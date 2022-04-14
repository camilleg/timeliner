#!/usr/bin/env ruby

# Compute wavfile from comments in timeliner-recording.txt.

Src = 'example/mono/marshal/mixed.wav'
SR = 16000

s = `grep ^# timeliner-recording.txt`.split(/\n/)
cFrame = `grep -v ^# timeliner-recording.txt|wc -l`.to_i

def tmp(i) "/run/shm/timeliner/tmp#{i}.wav" end
def tmps() "/run/shm/timeliner/tmp*.wav" end
`rm -f #{tmps}`

if true

  # This handles ONLY consecutive start commands.
  $secDstPrev = -1.0
  $secSrcPrev = -1.0
  s.each_with_index {|c,i|
    c = c.split
    $secSrc = c[8].to_f
    $secDst = c[14].to_f
    if ($secDstPrev >= 0.0)
      $secDur = $secDst - $secDstPrev
#     puts "prev dstBgn #$secDstPrev"
#     puts "curr dstBgn #{$secDst}"
#     puts "previous clip's dur #{$secDur}"
#     puts "Thus, clip src start #{$secSrcPrev}, dstBgn #{$secDstPrev}, dur #{$secDur}\n\n"
      `sox #{Src} #{tmp(i)} trim #{($secSrcPrev*SR).to_i}s #{($secDur*SR).to_i}s pad #{($secDstPrev*SR).to_i}s`
    end
    $secDstPrev = $secDst
    $secSrcPrev = $secSrc
#   puts "clip src start #{$secSrc}, dst #{$secDst}"
  }

  i = s.size
  $secVideo = cFrame/30.0
  $secDur = $secVideo - $secDst
# puts "    last clip's dur #{$secDur}"
# puts "Thus, clip src start #{$secSrc}, dst #{$secDst}, dur #{$secDur} dstEnd #{$secDst+$secDur}\n\n"
  `sox #{Src} #{tmp(i)} trim #{($secSrc*SR).to_i}s #{($secDur*SR).to_i}s pad #{($secDst*SR).to_i}s`

else

  # This handles alternating start and stop commands.
  # Join each start-stop pair of lines.
  clips = []
  s.each_slice(2) {|c| clips << c.join}
  clips.each_with_index {|c,i|
    # parse "# audio playback start from wavfile offset = 7.8 s, at screenshot-recording offset = 1.9 s# audio playback stop at offset = 8.2 s"
    c = c.split
    secSrc    = c[8].to_f
    secDst    = c[14].to_f
    secDur = c[-2].to_f
    puts "clip src start #{secSrc}, dst #{secDst}, dur #{secDur} "
    `sox #{Src} #{tmp(i)} trim #{(secSrc*SR).to_i}s #{(secDur*SR).to_i}s pad #{(secDst*SR).to_i}s`
  }

end

`sox -m #{tmps} /run/shm/timeliner/out.wav gain -n -1`
`rm -f #{tmps}`
