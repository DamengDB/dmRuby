module Dm
  class Client
    attr_reader :query_options, :read_timeout

    def self.default_query_options
      @default_query_options ||= {
        as: :hash,                   # the type of object you want each row back as; also supports :array (an array of values)
        cast_booleans: false,        # cast tinyint(1) fields as true/false in ruby
        symbolize_keys: false,       # return field names as symbols instead of strings
        cache_rows: true,            # tells to use its internal row cache for results
        cast: true,
      }
    end

    def initialize(opts = {})
      raise Dm::Error, "Options parameter must be a Hash" unless opts.is_a? Hash

      opts = Dm::Util.key_hash_as_symbols(opts)
      @query_options = self.class.default_query_options.dup
      @query_options.merge! opts
     
      # force the encoding to utf8

      user     = opts[:username] || opts[:user]
      pass     = opts[:password] || opts[:pass]
      server   = opts[:server] || opts[:host]
      schema   = opts[:schema]

      # Correct the data types before passing these values down to the C level
      user = user.to_s unless user.nil?
      pass = pass.to_s unless pass.nil?
      server = server.to_s unless server.nil?
      schema = schema.to_s unless schema.nil?
      self.charset_name = opts[:encoding] || 1
      connect user, pass, server, schema
    end

    def query(sql, options = {})
      Thread.handle_interrupt(::Dm::Util::TIMEOUT_ERROR_NEVER) do
        _query(sql, @query_options.merge(options))
      end
    end


    class << self
      private

      def local_offset
        ::Time.local(2010).utc_offset.to_r / 86400
      end
    end
  end
end
