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

require 'phusion_passenger'
require 'phusion_passenger/constants'
require 'phusion_passenger/console_text_template'
require 'phusion_passenger/platform_info'
require 'phusion_passenger/utils/ansi_colors'

# IMPORTANT: do not directly or indirectly require native_support; we can't compile
# it yet until we have a compiler, and installers usually check whether a compiler
# is installed.

module PhusionPassenger

# Abstract base class for text mode installers. Used by
# passenger-install-apache2-module and passenger-install-nginx-module.
#
# Subclasses must at least implement the #run_steps method which handles
# the installation itself.
#
# Usage:
#
#   installer = ConcereteInstallerClass.new(options...)
#   installer.run
class AbstractInstaller
	PASSENGER_WEBSITE = "https://www.phusionpassenger.com"
	PHUSION_WEBSITE = "www.phusion.nl"

	# Create an AbstractInstaller. All options will be stored as instance
	# variables, for example:
	#
	#   installer = AbstractInstaller.new(:foo => "bar")
	#   installer.instance_variable_get(:"@foo")   # => "bar"
	def initialize(options = {})
		@stdout = STDOUT
		@stderr = STDERR
		options.each_pair do |key, value|
			instance_variable_set(:"@#{key}", value)
		end
	end
	
	# Start the installation by calling the #install! method.
	def run
		before_install
		run_steps
		return true
	rescue Abort
		puts
		return false
	rescue PlatformInfo::RuntimeError => e
		new_screen
		puts "<red>An error occurred</red>"
		puts
		puts e.message
		exit 1
	ensure
		after_install
	end

protected
	class Abort < StandardError
	end
	
	class CommandError < Abort
	end
	
	
	def interactive?
		return !@auto
	end

	def non_interactive?
		return !interactive?
	end
	
	
	def before_install
		STDOUT.write(Utils::AnsiColors::DEFAULT_TERMINAL_COLOR)
		STDOUT.flush
	end
	
	def after_install
		STDOUT.write(Utils::AnsiColors::RESET)
		STDOUT.flush
	end
	
	def dependencies
		return [[], []]
	end
	
	def check_dependencies(show_new_screen = true)
		new_screen if show_new_screen
		puts "<banner>Checking for required software...</banner>"
		puts
		
		require 'phusion_passenger/platform_info/depcheck'
		specs, ids = dependencies
		runner = PlatformInfo::Depcheck::ConsoleRunner.new

		specs.each do |spec|
			PlatformInfo::Depcheck.load(spec)
		end
		ids.each do |id|
			runner.add(id)
		end

		if runner.check_all
			return true
		else
			puts
			puts "<red>Some required software is not installed.</red>"
			puts "But don't worry, this installer will tell you how to install them.\n"
			puts "<b>Press Enter to continue, or Ctrl-C to abort.</b>"
			if PhusionPassenger.originally_packaged?
				wait
			else
				wait(10)
			end

			line
			puts
			puts "<banner>Installation instructions for required software</banner>"
			puts
			runner.missing_dependencies.each do |dep|
				puts " * To install <yellow>#{dep.name}</yellow>:"
				puts "   #{dep.install_instructions}"
				puts
			end
			if respond_to?(:users_guide)
				puts "If the aforementioned instructions didn't solve your problem, then please take"
				puts "a look at the Users Guide:"
				puts
				puts "  <yellow>#{users_guide}</yellow>"
			end
			return false
		end
	end
	
	
	def use_stderr
		old_stdout = @stdout
		begin
			@stdout = STDERR
			yield
		ensure
			@stdout = old_stdout
		end
	end
	
	def print(text)
		@stdout.write(Utils::AnsiColors.ansi_colorize(text))
		@stdout.flush
	end
	
	def puts(text = nil)
		if text
			@stdout.puts(Utils::AnsiColors.ansi_colorize(text))
		else
			@stdout.puts
		end
		@stdout.flush
	end

	def puts_error(text)
		@stderr.puts(Utils::AnsiColors.ansi_colorize("<red>#{text}</red>"))
		@stderr.flush
	end
	
	def render_template(name, options = {})
		puts ConsoleTextTemplate.new({ :file => name }, options).result
	end
	
	def new_screen
		puts
		line
		puts
	end
	
	def line
		puts "--------------------------------------------"
	end
	
	def prompt(message, default_value = nil)
		done = false
		while !done
			print "#{message}: "
			
			if non_interactive? && default_value
				puts default_value
				return default_value
			end
			
			begin
				result = STDIN.readline
			rescue EOFError
				exit 2
			end
			result.strip!
			if result.empty?
				if default_value
					result = default_value
					done = true
				else
					done = !block_given? || yield(result)
				end
			else
				done = !block_given? || yield(result)
			end
		end
		return result
	end
	
	def prompt_confirmation(message)
		result = prompt("#{message} [y/n]") do |value|
			if value.downcase == 'y' || value.downcase == 'n'
				true
			else
				puts_error "Invalid input '#{value}'; please enter either 'y' or 'n'."
				false
			end
		end
		return result.downcase == 'y'
	end

	def wait(timeout = nil)
		if interactive?
			if timeout
				require 'timeout' unless defined?(Timeout)
				begin
					Timeout.timeout(timeout) do
						STDIN.readline
					end
				rescue Timeout::Error
					# Do nothing.
				end
			else
				STDIN.readline
			end
		end
	rescue Interrupt
		raise Abort
	end
	
	
	def sh(*args)
		puts "# #{args.join(' ')}"
		result = system(*args)
		if result
			return true
		elsif $?.signaled? && $?.termsig == Signal.list["INT"]
			raise Interrupt
		else
			return false
		end
	end
	
	def sh!(*args)
		if !sh(*args)
			puts_error "*** Command failed: #{args.join(' ')}"
			raise CommandError
		end
	end
	
	def rake(*args)
		require 'phusion_passenger/platform_info/ruby'
		if !PlatformInfo.rake_command
			puts_error 'Cannot find Rake.'
			raise Abort
		end
		sh("#{PlatformInfo.rake_command} #{args.join(' ')}")
	end

	def rake!(*args)
		require 'phusion_passenger/platform_info/ruby'
		if !PlatformInfo.rake_command
			puts_error 'Cannot find Rake.'
			raise Abort
		end
		sh!("#{PlatformInfo.rake_command} #{args.join(' ')}")
	end
	
	def download(url, output)
		if PlatformInfo.find_command("wget")
			return sh("wget", "-O", output, url)
		else
			return sh("curl", url, "-f", "-L", "-o", output)
		end
	end
end

end # module PhusionPassenger
