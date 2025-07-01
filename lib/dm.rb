# frozen_string_literal: true
require 'date'
require 'bigdecimal'
require_relative "dm/version"



module Dm
  
  begin
    require "dm/dm/dm_lib_path"
  rescue LoadError
    # rake-compiler doesn't use regular "make install", but uses it's own install tasks.
    DM_LIB_PATH = false
  end
  
  add_dll_path = proc do |path, &block|
    if RUBY_PLATFORM =~/(mswin|mingw)/i && path && File.exist?(path)
      begin
        require 'ruby_installer/runtime'
        RubyInstaller::Runtime.add_dll_directory(path, &block)
      rescue LoadError
        old_path = ENV['PATH']
        ENV['PATH'] = "#{path};#{old_path}"
        block.call
        ENV['PATH'] = old_path
      end
    else
      # No need to set a load path manually - it's set as library rpath.
      block.call
    end
  end
  
  # Add a load path to the one retrieved from pg_config
  add_dll_path.call(DM_LIB_PATH) do
    require 'dm/error'
    require 'dm/dm_ext'
    require 'dm/result'
    require 'dm/client'
    require 'dm/field'
    require 'dm/statement'
  end
  
end

# For holding utility methods
module Dm
  module Util
    #
    # Rekey a string-keyed hash with equivalent symbols.
    #
    def self.key_hash_as_symbols(hash)
      return nil unless hash

      Hash[hash.map { |k, v| [k.to_sym, v] }]
    end
    require 'timeout'
    TIMEOUT_ERROR_CLASS = if defined?(::Timeout::ExitException)
      ::Timeout::ExitException
    else
      ::Timeout::Error
    end
    TIMEOUT_ERROR_NEVER = { TIMEOUT_ERROR_CLASS => :never }.freeze
  end
end
