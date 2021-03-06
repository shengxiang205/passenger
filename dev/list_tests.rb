#!/usr/bin/env ruby
# Lists the test names in the given .cpp test file.
require_relative '../lib/phusion_passenger/utils/ansi_colors'

include PhusionPassenger::Utils::AnsiColors

def extract_category_name(occurrence)
	occurrence =~ / (.+) /
	return $1
end

def extract_test_name(occurrence)
	occurrence = occurrence.sub(/.*?\((.+)\).*/m, '\1')
	occurrence.gsub!(/"\n[ \t]*"/m, '')
	occurrence.sub!(/\A"/, '')
	occurrence.sub!(/"\Z/, '')
	return occurrence
end

def start(filename)
	STDOUT.write(DEFAULT_TERMINAL_COLOR)
	begin
		occurrences = File.read(filename).scan(%r{/\*\*\*\*\* .+? \*\*\*\*\*/|set_test_name\(.+?\);}m)
		occurrences.each do |occurrence|
			if occurrence =~ %r{\A/}
				puts ansi_colorize("<b>" + extract_category_name(occurrence) + "</b>")
			else
				puts "  " + extract_test_name(occurrence)
			end
		end
	ensure
		STDOUT.write(RESET)
	end
end

start(ARGV[0])
