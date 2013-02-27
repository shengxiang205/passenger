# encoding: binary
#  Phusion Passenger - https://www.phusionpassenger.com/
#  Copyright (c) 2011, 2012 Phusion
#
#  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy
#  of this software and associated documentation files (the "Software"), to deal
#  in the Software without restriction, including without limitation the rights
#  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
#  copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in
#  all copies or substantial portions of the Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
#  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
#  THE SOFTWARE.

require 'phusion_passenger/public_api'
require 'phusion_passenger/debug_logging'

module PhusionPassenger

# Provides shared functions for loader and preloader apps.
module LoaderSharedHelpers
	extend self

	# To be called by the (pre)loader as soon as possible.
	def init
		Thread.main[:name] = "Main thread"
		# We don't dump PATH info because at this point it's
		# unlikely to be changed.
		dump_ruby_environment
	end

	# To be called whenever the (pre)loader is about to abort with an error.
	def about_to_abort(exception = nil)
		dump_all_information
	end

	def to_boolean(value)
		return !(value.nil? || value == false || value == "false")
	end
	
	def sanitize_spawn_options(options)
		defaults = {
			"app_type"         => "rack",
			"environment"      => "production",
			"print_exceptions" => true
		}
		options = defaults.merge(options)
		options["app_group_name"]            = options["app_root"] if !options["app_group_name"]
		options["print_exceptions"]          = to_boolean(options["print_exceptions"])
		options["analytics"]                 = to_boolean(options["analytics"])
		options["show_version_in_header"]    = to_boolean(options["show_version_in_header"])
		options["log_level"]                 = options["log_level"].to_i if options["log_level"]
		# TODO: smart spawning is not supported when using ruby-debug. We should raise an error
		# in this case.
		options["debugger"]     = to_boolean(options["debugger"])
		options["spawn_method"] = "direct" if options["debugger"]
		
		return options
	end

	def dump_all_information
		dump_ruby_environment
		dump_envvars
		dump_system_memory_stats
	end

	def dump_ruby_environment
		if dir = ENV['PASSENGER_DEBUG_DIR']
			File.open("#{dir}/ruby_info", "w") do |f|
				f.puts "RUBY_VERSION = #{RUBY_VERSION}"
				f.puts "RUBY_PLATFORM = #{RUBY_PLATFORM}"
				f.puts "RUBY_ENGINE = #{defined?(RUBY_ENGINE) ? RUBY_ENGINE : 'nil'}"
			end
			File.open("#{dir}/load_path", "w") do |f|
				$LOAD_PATH.each do |path|
					f.puts path
				end
			end
			File.open("#{dir}/loaded_libs", "w") do |f|
				$LOADED_FEATURES.each do |filename|
					f.puts filename
				end
			end

			# We write to these files last because the 'require' calls can fail.
			require 'rbconfig' if !defined?(RbConfig::CONFIG)
			File.open("#{dir}/rbconfig", "w") do |f|
				RbConfig::CONFIG.each_pair do |key, value|
					f.puts "#{key} = #{value}"
				end
			end
			require 'rubygems' if !defined?(Gem)
			File.open("#{dir}/ruby_info", "a") do |f|
				f.puts "RubyGems version = #{Gem::VERSION}"
			end
			File.open("#{dir}/activated_gems", "w") do |f|
				if Gem.respond_to?(:loaded_specs)
					Gem.loaded_specs.each_pair do |name, spec|
						f.puts "#{name} => #{spec.version}"
					end
				else
					f.puts "Unable to query this information; incompatible RubyGems API."
				end
			end
		end
	rescue SystemCallError
		# Don't care.
	end

	def dump_envvars
		if dir = ENV['PASSENGER_DEBUG_DIR']
			File.open("#{dir}/envvars", "w") do |f|
				ENV.each_pair do |key, value|
					f.puts "#{key} = #{value}"
				end
			end
		end
	rescue SystemCallError
		# Don't care.
	end

	def dump_system_memory_stats
		if dir = ENV['PASSENGER_DEBUG_DIR']
			File.open("#{dir}/sysmemory", "w") do |f|
				f.write(`"#{PhusionPassenger.helper_scripts_dir}/system-memory-stats.py"`)
			end
		end
	rescue SystemCallError
		# Don't care.
	end
	
	# Prepare an application process using rules for the given spawn options.
	# This method is to be called before loading the application code.
	#
	# +startup_file+ is the application type's startup file, e.g.
	# "config/environment.rb" for Rails apps and "config.ru" for Rack apps.
	# +options+ are the spawn options that were given.
	#
	# This function may modify +options+. The modified options are to be
	# passed to the request handler.
	def before_loading_app_code_step1(startup_file, options)
		DebugLogging.log_level = options["log_level"] if options["log_level"]

		# Instantiate the analytics logger if requested. Can be nil.
		require 'phusion_passenger/analytics_logger'
		options["analytics_logger"] = AnalyticsLogger.new_from_options(options)
	end
	
	def run_load_path_setup_code(options)
		# rack-preloader.rb depends on the 'rack' library, but the app
		# might want us to use a bundled version instead of a
		# gem/apt-get/yum/whatever-installed version. Therefore we must setup
		# the correct load paths before requiring 'rack'.
		#
		# The most popular tool for bundling dependencies is Bundler. Bundler
		# works as follows:
		# - If the bundle is locked then a file .bundle/environment.rb exists
		#   which will setup the load paths.
		# - If the bundle is not locked then the load paths must be set up by
		#   calling Bundler.setup.
		# - Rails 3's boot.rb automatically loads .bundle/environment.rb or
		#   calls Bundler.setup if that's not available.
		# - Other Rack apps might not have a boot.rb but we still want to setup
		#   Bundler.
		# - Some Rails 2 apps might have explicitly added Bundler support.
		#   These apps call Bundler.setup in their preinitializer.rb.
		#
		# So the strategy is as follows:
		
		# Our strategy might be completely unsuitable for the app or the
		# developer is using something other than Bundler, so we let the user
		# manually specify a load path setup file.
		if options["load_path_setup_file"]
			require File.expand_path(options["load_path_setup_file"])
		
		# The app developer may also override our strategy with this magic file.
		elsif File.exist?('config/setup_load_paths.rb')
			require File.expand_path('config/setup_load_paths')
		
		# If the Bundler lock environment file exists then load that. If it
		# exists then there's a 99.9% chance that loading it is the correct
		# thing to do.
		elsif File.exist?('.bundle/environment.rb')
			require File.expand_path('.bundle/environment')
		
		# If the Bundler environment file doesn't exist then there are two
		# possibilities:
		# 1. Bundler is not used, in which case we don't have to do anything.
		# 2. Bundler *is* used, but the gems are not locked and we're supposed
		#    to call Bundler.setup.
		#
		# The existence of Gemfile indicates whether (2) is true:
		elsif File.exist?('Gemfile')
			# In case of Rails 3, config/boot.rb already calls Bundler.setup.
			# However older versions of Rails may not so loading boot.rb might
			# not be the correct thing to do. To be on the safe side we
			# call Bundler.setup ourselves; calling Bundler.setup twice is
			# harmless. If this isn't the correct thing to do after all then
			# there's always the load_path_setup_file option and
			# setup_load_paths.rb.
			require 'rubygems'
			require 'bundler/setup'
		end
		
		# Bundler might remove Phusion Passenger from the load path in its zealous
		# attempt to un-require RubyGems, so here we put Phusion Passenger back
		# into the load path. This must be done before loading the app's startup
		# file because the app might require() Phusion Passenger files.
		if !$LOAD_PATH.include?(PhusionPassenger.ruby_libdir)
			$LOAD_PATH.unshift(PhusionPassenger.ruby_libdir)
			$LOAD_PATH.uniq!
		end
		
		
		# !!! NOTE !!!
		# If the app is using Bundler then any dependencies required past this
		# point must be specified in the Gemfile. Like ruby-debug if debugging is on...
	end
	
	def before_loading_app_code_step2(options)
		# Do nothing.
	end
	
	# This method is to be called after loading the application code but
	# before forking a worker process.
	def after_loading_app_code(options)
		# Even though run_load_path_setup_code() restores the Phusion Passenger
		# load path after setting up Bundler, the app itself might also
		# remove Phusion Passenger from the load path for whatever reason,
		# so here we restore the load path again.
		if !$LOAD_PATH.include?(PhusionPassenger.ruby_libdir)
			$LOAD_PATH.unshift(PhusionPassenger.ruby_libdir)
			$LOAD_PATH.uniq!
		end
		
		# Post-install framework extensions. Possibly preceded by a call to
		# PhusionPassenger.install_framework_extensions!
		if defined?(::Rails) && !defined?(::Rails::VERSION)
			require 'rails/version'
		end
	end
	
	def create_socket_address(protocol, address)
		if protocol == 'unix'
			return "unix:#{address}"
		elsif protocol == 'tcp'
			return "tcp://#{address}"
		else
			raise ArgumentError, "Unknown protocol '#{protocol}'"
		end
	end
	
	def advertise_sockets(output, request_handler)
		request_handler.server_sockets.each_pair do |name, options|
			output.puts "!> socket: #{name};#{options[:address]};#{options[:protocol]};#{options[:concurrency]}"
		end
	end
	
	# To be called before the request handler main loop is entered, but after the app
	# startup file has been loaded. This function will fire off necessary events
	# and perform necessary preparation tasks.
	#
	# +forked+ indicates whether the current worker process is forked off from
	# an ApplicationSpawner that has preloaded the app code.
	# +options+ are the spawn options that were passed.
	def before_handling_requests(forked, options)
		if forked
			# Reseed pseudo-random number generator for security reasons.
			srand
		end

		if options["process_title"] && !options["process_title"].empty?
			$0 = options["process_title"] + ": " + options["app_group_name"]
		end

		if forked && options["analytics_logger"]
			options["analytics_logger"].clear_connection
		end
		
		# If we were forked from a preloader process then clear or
		# re-establish ActiveRecord database connections. This prevents
		# child processes from concurrently accessing the same
		# database connection handles.
		if forked && defined?(ActiveRecord::Base)
			if ActiveRecord::Base.respond_to?(:clear_all_connections!)
				ActiveRecord::Base.clear_all_connections!
			elsif ActiveRecord::Base.respond_to?(:clear_active_connections!)
				ActiveRecord::Base.clear_active_connections!
			elsif ActiveRecord::Base.respond_to?(:connected?) &&
			      ActiveRecord::Base.connected?
				ActiveRecord::Base.establish_connection
			end
		end
		
		# Fire off events.
		PhusionPassenger.call_event(:starting_worker_process, forked)
		if options["pool_account_username"] && options["pool_account_password_base64"]
			password = options["pool_account_password_base64"].unpack('m').first
			PhusionPassenger.call_event(:credentials,
				options["pool_account_username"], password)
		else
			PhusionPassenger.call_event(:credentials, nil, nil)
		end
	end
	
	# To be called after the request handler main loop is exited. This function
	# will fire off necessary events perform necessary cleanup tasks.
	def after_handling_requests
		PhusionPassenger.call_event(:stopping_worker_process)
	end
end

end # module PhusionPassenger
