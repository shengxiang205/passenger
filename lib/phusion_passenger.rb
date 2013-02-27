# encoding: utf-8
#  Phusion Passenger - https://www.phusionpassenger.com/
#  Copyright (c) 2010-2013 Phusion
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

module PhusionPassenger
	FILE_LOCATION = File.expand_path(__FILE__)
	
	
	###### Version numbers ######
	
	PACKAGE_NAME = 'passenger'
	
	# Phusion Passenger version number. Don't forget to edit ext/common/Constants.h too.
	VERSION_STRING = '3.9.4.rc2'
	
	PREFERRED_NGINX_VERSION = '1.2.7'
	PREFERRED_PCRE_VERSION  = '8.31'
	STANDALONE_INTERFACE_VERSION  = 1
	
	
	###### Directories ######
	
	GLOBAL_NAMESPACE_DIRNAME            = "phusion-passenger"
	GLOBAL_STANDALONE_NAMESPACE_DIRNAME = "passenger-standalone"
	# Subdirectory under $HOME to use for storing stuff.
	USER_NAMESPACE_DIRNAME              = ".passenger"
	
	# Directories in which to look for plugins.
	PLUGIN_DIRS = [
		"/usr/share/#{GLOBAL_NAMESPACE_DIRNAME}/plugins",
		"/usr/local/share/#{GLOBAL_NAMESPACE_DIRNAME}/plugins",
		"~/#{USER_NAMESPACE_DIRNAME}/plugins"
	]
	
	# Directory under $HOME for storing Phusion Passenger Standalone runtime files.
	LOCAL_STANDALONE_RESOURCE_DIR  = File.join(USER_NAMESPACE_DIRNAME, "standalone")
	
	# System-wide directory for storing Phusion Passenger Standalone runtime files.
	GLOBAL_STANDALONE_RESOURCE_DIR = "/var/lib/#{GLOBAL_STANDALONE_NAMESPACE_DIRNAME}".freeze
	
	NATIVELY_PACKAGED_BIN_DIR                = "/usr/bin".freeze
	NATIVELY_PACKAGED_AGENTS_DIR             = "/usr/lib/#{GLOBAL_NAMESPACE_DIRNAME}/agents".freeze
	NATIVELY_PACKAGED_HELPER_SCRIPTS_DIR     = "/usr/share/#{GLOBAL_NAMESPACE_DIRNAME}/helper-scripts".freeze
	NATIVELY_PACKAGED_RESOURCES_DIR          = "/usr/share/#{GLOBAL_NAMESPACE_DIRNAME}".freeze
	NATIVELY_PACKAGED_DOC_DIR                = "/usr/share/doc/#{GLOBAL_NAMESPACE_DIRNAME}".freeze
	NATIVELY_PACKAGED_RUNTIME_LIBDIR         = "/usr/lib/#{GLOBAL_NAMESPACE_DIRNAME}".freeze
	NATIVELY_PACKAGED_HEADER_DIR             = "/usr/include/#{GLOBAL_NAMESPACE_DIRNAME}".freeze
	NATIVELY_PACKAGED_APACHE2_MODULE         = "/usr/lib/apache2/modules/mod_passenger.so".freeze
	
	# Follows the logic of ext/common/ResourceLocator.h, so don't forget to modify that too.
	def self.locate_directories(source_root_or_location_configuration_file = nil)
		source_root_or_location_configuration_file ||= find_location_configuration_file
		root_or_file = @source_root = source_root_or_location_configuration_file
		
		if root_or_file && File.file?(root_or_file)
			filename = root_or_file
			options  = {}
			in_locations_section = false
			File.open(filename, 'r') do |f|
				while !f.eof?
					line = f.readline
					line.strip!
					next if line.empty?
					if line =~ /\A\[(.+)\]\Z/
						in_locations_section = $1 == 'locations'
					elsif in_locations_section && line =~ /=/
						key, value = line.split(/ *= */, 2)
						options[key.freeze] = value.freeze
					end
				end
			end
			
			@natively_packaged     = get_bool_option(filename, options, 'natively_packaged')
			@bin_dir               = get_option(filename, options, 'bin').freeze
			@agents_dir            = get_option(filename, options, 'agents').freeze
			@helper_scripts_dir    = get_option(filename, options, 'helper_scripts').freeze
			@resources_dir         = get_option(filename, options, 'resources').freeze
			@doc_dir               = get_option(filename, options, 'doc').freeze
			@apache2_module_path   = get_option(filename, options, 'apache2_module').freeze
			@ruby_extension_source_dir = get_option(filename, options, 'ruby_extension_source').freeze
		else
			@source_root           = File.dirname(File.dirname(FILE_LOCATION))
			@natively_packaged     = false
			@bin_dir               = "#{@source_root}/bin".freeze
			@agents_dir            = "#{@source_root}/agents".freeze
			@helper_scripts_dir    = "#{@source_root}/helper-scripts".freeze
			@resources_dir         = "#{@source_root}/resources".freeze
			@doc_dir               = "#{@source_root}/doc".freeze
			@apache2_module_path   = "#{@source_root}/libout/apache2/mod_passenger.so".freeze
			@ruby_extension_source_dir = "#{@source_root}/ext/ruby"
		end
	end
	
	# Returns whether this Phusion Passenger installation is in the 'originally packaged'
	# configuration (as opposed to the 'natively packaged' configuration.
	def self.originally_packaged?
		return !@natively_packaged
	end

	def self.natively_packaged?
		return @natively_packaged
	end

	# When originally packaged, returns the source root.
	# When natively packaged, returns the location of the location configuration file.
	def self.source_root
		return @source_root
	end
	
	def self.bin_dir
		return @bin_dir
	end
	
	def self.agents_dir
		return @agents_dir
	end
	
	def self.helper_scripts_dir
		return @helper_scripts_dir
	end
	
	def self.resources_dir
		return @resources_dir
	end
	
	def self.doc_dir
		return @doc_dir
	end
	
	def self.ruby_libdir
		@libdir ||= File.dirname(FILE_LOCATION)
	end
	
	def self.apache2_module_path
		return @apache2_module_path
	end

	def self.ruby_extension_source_dir
		return @ruby_extension_source_dir
	end
	
	
	###### Other resource locations ######
	
	STANDALONE_BINARIES_URL_ROOT  = "http://standalone-binaries.modrails.com"
	
	
	if !$LOAD_PATH.include?(ruby_libdir)
		$LOAD_PATH.unshift(ruby_libdir)
		$LOAD_PATH.uniq!
	end


private
	def self.find_location_configuration_file
		filename = ENV['PASSENGER_LOCATION_CONFIGURATION_FILE']
		return filename if filename && !filename.empty?

		filename = File.dirname(FILE_LOCATION) + "/phusion_passenger/locations.ini"
		return filename if filename && File.exist?(filename)

		require 'etc' if !defined?(Etc)
		begin
			home_dir = Etc.getpwuid(Process.uid).dir
		rescue ArgumentError
			# Unknown user.
			home_dir = ENV['HOME']
		end
		if home_dir && !home_dir.empty?
			filename = "#{home_dir}/.passenger/locations.ini"
			return filename if File.exist?(filename)
		end

		filename = "/etc/#{GLOBAL_NAMESPACE_DIRNAME}/locations.ini"
		return filename if File.exist?(filename)

		return nil
	end

	def self.get_option(filename, options, key, required = true)
		value = options[key]
		if value
			return value
		elsif required
			raise "Option '#{key}' missing in file '#{filename}'"
		else
			return nil
		end
	end
	
	def self.get_bool_option(filename, options, key)
		value = get_option(filename, options, key)
		return value == 'yes' || value == 'true' || value == 'on' || value == '1'
	end
end if !defined?(PhusionPassenger::VERSION_STRING)
