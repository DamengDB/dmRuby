# Loaded by script/console. Land helpers here.

Pry.config.prompt = lambda do |context, *|
  "[ruby-dm] #{context}> "
end
