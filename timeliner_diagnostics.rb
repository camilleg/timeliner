if Debug
  def quit(_) $stdout.puts "#$0 ABORT: #{_}\n\n"; exit 1; end
  def warn(_) $stdout.puts "#$0 WARN: #{_}\n" end
  def info(_) $stdout.puts "#$0 info: #{_}" end
else
  # Get user's attention: report only fatality.
  # They'll ignore more than one diagnostic.
  def quit(_) $stdout.puts "#$0 ABORT: #{_}"; exit 1; end
  def warn(_) quit(_) end
  def info(_) end
end

def requireGem r, gem=r
  require r
  rescue Exception  
    quit "First, sudo gem install #{gem}"
end

# If a ruby script was retrieved as foo.txt from a webserver
# (so the webserver wouldn't mistakenly run it as a cgi script),
# remind the user to rename it from foo_rb.txt to foo.rb.
def forceFilename n
  quit "rename #$0 to #{n}" if File.basename($0) != n
end
