require 'pp'
require 'mkmf'


if ENV['MAINTAINER_MODE']
	$stderr.puts "Maintainer mode enabled."
	$CFLAGS <<
		' -Wall' <<
		' -g3' <<
		' -DDEBUG' <<
		' -pedantic'
	$LDFLAGS <<
		' -g3'
end

if enable_config("windows-cross")
	# Avoid dependency to external libgcc.dll on x86-mingw32
	$LDFLAGS << " -static-libgcc"
	# Don't use pg_config for cross build, but --with-pg-* path options
	dir_config 'dm'
end

if ENV['DM_HOME']
	if /mswin|mingw/ =~ RUBY_PLATFORM
		incdir = ENV['DM_HOME'] + "\\include"
		libdir = ENV['DM_HOME'] + "\\bin"
	else
		incdir = ENV['DM_HOME'] + '/include'
		libdir = ENV['DM_HOME'] + '/bin'
	end
	dlldir = libdir
    dir_config('dm',incdir,libdir)
	# Try to use runtime path linker option, even if RbConfig doesn't know about it.
	# The rpath option is usually set implicit by dir_config(), but so far not
	# on MacOS-X.
	if dlldir && RbConfig::CONFIG["RPATHFLAG"].to_s.empty?
		append_ldflags "-Wl,-rpath,#{dlldir.quote}"
	end

	if /mswin/ =~ RUBY_PLATFORM
		$libs = append_library($libs, 'ws2_32')
	end

else
	# Native build
	incdir, libdir = dir_config 'dm'
	dlldir = libdir

	# Try to use runtime path linker option, even if RbConfig doesn't know about it.
	# The rpath option is usually set implicit by dir_config(), but so far not
	# on MacOS-X.
	if dlldir && RbConfig::CONFIG["RPATHFLAG"].to_s.empty?
		append_ldflags "-Wl,-rpath,#{dlldir.quote}"
	end

	if /mswin/ =~ RUBY_PLATFORM
		$libs = append_library($libs, 'ws2_32')
	end
end

$stderr.puts "Using dpi from #{dlldir}"

File.write("dm_lib_path.rb", <<-EOT)
module Dm
	DM_LIB_PATH = #{dlldir.inspect}
end
EOT
$INSTALLFILES = {
	"./dm_lib_path.rb" => "$(RUBYLIBDIR)/dm/"
}

if RUBY_VERSION >= '2.3.0' && /solaris/ =~ RUBY_PLATFORM
	append_cppflags( '-D__EXTENSIONS__' )
end

begin
	find_header( 'DPI.h' ) or abort "Can't find the DPI.h header"
	find_header( 'DPIext.h' ) or abort "Can't find the DPIext.h header"
	find_header( 'DPItypes.h' ) or abort "Can't find the DPItypes.h header"

	abort "Can't find the DM library (dpi)" unless
		have_library( 'dmdpi', 'dpi_login' ) ||
		have_library( 'libdmdpi', 'dpi_login' ) 

rescue SystemExit
	$stderr.puts <<-EOT
*****************************************************************************

Unable to find dm library.

pelease set library paths manually with:
  gem install dm -- --with-dm-include=/path/to/DPI.h/ --with-pg-lib=/path/to/libdmdpi.so/

EOT
	raise
end

if /mingw/ =~ RUBY_PLATFORM && RbConfig::MAKEFILE_CONFIG['CC'] =~ /gcc/
	# Work around: https://sourceware.org/bugzilla/show_bug.cgi?id=22504
	checking_for "workaround gcc version with link issue" do
		`#{RbConfig::MAKEFILE_CONFIG['CC']} --version`.chomp =~ /\s(\d+)\.\d+\.\d+(\s|$)/ &&
			$1.to_i >= 6 &&
			have_library(':dmdpi.lib') # Prefer linking to libpq.lib over libpq.dll if available
	end
end

have_func 'timegm'
have_func 'rb_gc_adjust_memory_usage' # since ruby-2.4
have_func 'rb_gc_mark_movable' # since ruby-2.7
have_func 'rb_io_wait' # since ruby-3.0
have_func 'rb_io_descriptor' # since ruby-3.1

# unistd.h confilicts with ruby/win32.h when cross compiling for win32 and ruby 1.9.1
have_header 'unistd.h'
have_header 'inttypes.h'
have_header('ruby/fiber/scheduler.h') if RUBY_PLATFORM=~/mingw|mswin/
checking_for "C99 variable length arrays" do
	$defs.push( "-DHAVE_VARIABLE_LENGTH_ARRAYS" ) if try_compile('void test_vla(int l){ int vla[l]; }')
end

create_header()
create_makefile( "dm/dm_ext" )

