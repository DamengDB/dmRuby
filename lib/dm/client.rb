module Dm
  class Client
    attr_reader :query_options, :read_timeout

    def self.default_query_options
      @default_query_options ||= {
        as: :hash,                   # the type of object you want each row back as; also supports :array (an array of values)
        async: false,                # don't wait for a result after sending the query, you'll have to monitor the socket yourself then eventually call Mysql2::Client#async_result
        cast_booleans: false,        # cast tinyint(1) fields as true/false in ruby
        symbolize_keys: false,       # return field names as symbols instead of strings
        database_timezone: :local,   # timezone Mysql2 will assume datetime objects are stored in
        application_timezone: nil,   # timezone Mysql2 will convert to before handing the object back to the caller
        cache_rows: true,            # tells Mysql2 to use its internal row cache for results
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

      # Correct the data types before passing these values down to the C level
      user = user.to_s unless user.nil?
      pass = pass.to_s unless pass.nil?
      server = server.to_s unless server.nil?
      self.charset_name = opts[:encoding] || 1
      connect user, pass, server
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
