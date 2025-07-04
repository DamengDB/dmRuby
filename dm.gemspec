# frozen_string_literal: true

require_relative "lib/dm/version"

Dm::GEMSPEC = Gem::Specification.new do |s|
  s.name = 'ruby-dm'
  s.version = Dm::VERSION
  s.authors = ['sunbiao']
  s.license = "MIT"
  s.email = ['sunbiao@dameng.com']
  s.extensions = ["ext/extconf.rb"]
  s.homepage = 'https://www.dameng.com/'
  s.rdoc_options = ["--charset=UTF-8"]
  s.summary = 'A simple, fast DM library for Ruby, binding to dmdpi'
  s.metadata = {
    'bug_tracker_uri'   => "https://www.dameng.com/",
    'changelog_uri'     => "https://www.dameng.com/",
    'documentation_uri' => "https://www.dameng.com/",
    'homepage_uri'      => s.homepage,
    'source_code_uri'   => "https://www.dameng.com/",
  }
  s.required_ruby_version = '>= 2.0.0'

  s.files = Dir.glob('ext/*')+Dir.glob('lib/*')+Dir.glob('lib/dm/*')+Dir.glob('./*')

  s.metadata['msys2_mingw_dependencies'] = 'libmariadbclient'

  s.add_runtime_dependency 'bigdecimal'
end
