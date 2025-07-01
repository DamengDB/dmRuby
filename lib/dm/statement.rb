module Dm
  class Statement
    def execute(*args, **kwargs)
      Thread.handle_interrupt(::Dm::Util::TIMEOUT_ERROR_NEVER) do
        _execute(*args, **kwargs)
      end
    end
  end
end
