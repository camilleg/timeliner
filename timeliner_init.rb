#!/usr/bin/ruby -Ku
Debug = false

require 'rubygems'

require 'ftools'
require 'set'
require 'rspec'

$LOAD_PATH << File.dirname(File.expand_path(__FILE__)) # Show require where my sisters are, when I'm called from another dir.
require 'timeliner_diagnostics'
forceFilename 'timeliner_init.rb'

case ARGV.size
when 0
  dirMarshal, configfile = '.timeliner_marshal', "timeliner_config.txt"
when 1
  dirMarshal, configfile = ARGV[0], "timeliner_config.txt"
when 2
  dirMarshal, configfile = ARGV
else
  quit "Usage: #$0 [dirMarshal [configfile]]"
end
configfile = File.expand_path configfile
dirMarshal = File.expand_path dirMarshal
configfile.freeze
dirMarshal.freeze
Dir.chdir "#{dirMarshal}/.." rescue Dir.mkdir dirMarshal rescue quit "failed to make subdirectory #{dirMarshal} in directory #{Dir.pwd}."

include Math

require 'timeliner_common'

def doEggs wav, wavInput, eggDir
  secEggMax = `sfinfo #{wavInput}|grep Duration|grep \ seconds`.split[1].to_f
  quit "empty input soundfile #{wavInput}" if secEggMax <= 0.0
  eggDir.chomp!
  quit "no easter-egg directory called #{eggDir}" if !File.directory? eggDir
  eggFiles = Dir.entries(eggDir)[2..-1]
  quit "empty easter-egg directory #{eggDir}" if eggFiles.empty?
  eggNames = eggFiles.select {|f| f =~ /.wav/} .map {|e| "#{eggDir}/#{e}" }
  quit "no .wav files in easter-egg directory #{eggDir}" if eggNames.empty?
  raweggs = [] # Array of arrays of s16 samples.
  eggNames.each {|e|
    # fails if e includes whitespace
    info "parsing #{e}"
    raweggs << `sox -V1 #{e} -r #{SR} -s -2 -c 1 -t raw - ` .unpack('s*')	# -V1 hides warnings of clipping.
    raweggs[-1].size .should > 0
  }
  secEggShortest = raweggs.map {|e| e.size} .min / SR.to_f
  sampEggLongest = raweggs.map {|e| e.size} .max
  secEggMax -= sampEggLongest/SR.to_f
  quit "Input soundfile #{wavInput} is shorter than one of the easter eggs, which lasts #{sampEggLongest/SR.to_f} seconds." if secEggMax < 0.0

  info "normalizing eggs"
  raweggs.map! {|e| m = 32767.0/e.max; e.map {|s| s*m}}

  secPerEgg = [25.0, secEggMax/3.0].min
  cegg = (secEggMax/secPerEgg).to_i
  cegg .should > 0
  info "adding #{cegg} eastereggs to #{wavInput}, yielding #{wav}"

  info "laying eggs"
  # Accumulate eggs at random offsets into a buffer.
  rawegged = Array.new((secEggMax*SR + sampEggLongest + 2).to_i, 0)	# 2 is chickenfactor (ha,ha).
  eggs = [] # Array of [start, end, which_egg, amplitude] tuples.  Start-end intervals will be disjoint.
  srand 42  # Same sequence of rand for each run, just to reduce churn in the git repository.
  cegg.times {|iEgg|
    iEgg %= raweggs.size
    begin
      secDelay = rand * secEggMax
      secDur = raweggs[iEgg].size.to_f/SR
      secEnd = secDelay + secDur
      # Forbid overlapping eggs.
      # New egg [secDelay,secEnd] is [a,b].  Other eggs are [c,d].  Constrain b<c or d<a.
      # Stronger: constrain egg to be at least secEggShortest away from other eggs,
      # so subjects don't confuse one egg with two.
    end until eggs.all? {|c,d,_| secEnd+secEggShortest<c || d<secDelay-secEggShortest}
    sampleDelay = (secDelay * SR).to_i
    ampl = 0.1 + 0.4 * rand
    ampl = 0.9 if rand > 0.01			# A very few eggs are really loud.
    raweggs[iEgg].each_with_index {|sample,i|
      rawegged[sampleDelay+i] += sample * ampl
    }
    eggs << [secDelay, secEnd, iEgg, ampl]
    # info "egg #{iEgg}" # "during seconds #{secDelay} to #{secEnd}"
  }
  eggs = eggs.sort_by {|secStart,*| secStart}
  rawegged.slice!(eggs[-1][1]*SR + 1, -1)		# Crop silence after last egg.

  info "mixing eggs"
  info "Ignore sox's warning that the input file wasn't mono." if $channels != 1
  # works at 3 hours, but at 6 hours, OOM in pack 's*':
  IO.popen("sox -m    -v 0.1 -r #{SR} -s -2 -c 1 -t raw -    -v 1.0 #{wavInput}    #{wav}", "w+") {|f| f.puts rawegged.pack('s*')}

  [eggNames, eggs]
end

quit "No HTK feature config file #{configfile}" if !File.exist? configfile
cfg = File.readlines(configfile) .map &:strip		# Strip whitespace.
cfg.delete_if {|l| l =~ /^#/ || l.empty? }		# Strip comments and blank lines.
cfg.map! {|l| l.sub(/#.*/, "").strip }			# Strip trailing comments.
# Parse (and delete) key-value pairs.
$numchans = 62
cfg.each {|l|
  if l =~ /\w+\s*=\s*\S+/
    key,value = l.split('=') .map &:strip
    case key
    when 'wav'
      warn "Config file #{configfile}: duplicate wav" if $wavSrc
      $wavSrc = value
      info "Source audio is #$wavSrc."
    when 'egg'
      warn "Config file #{configfile}: duplicate egg" if $eggSrc
      $eggSrc = value
      info "Source egg dir is #$eggSrc."
    when 'numchans'
      $numchans = value.to_i
      info "Filterbank will have #$numchans channels."
    else
      warn "Config file #{configfile}: key=value syntax: unrecognized key #{key}"
    end
  end
}
quit "Config file #{configfile}: no line 'wav=/foo/bar.wav'" if !$wavSrc
info "Config file #{configfile}: no line 'egg=/foo/bar', thus no eggs will be made" if !$eggSrc
cfg.delete_if {|l| l =~ /\w+=\S+/ }

quit "no input soundfile #$wavSrc" if !File.exist? $wavSrc
wav = "#{dirMarshal}/mixed.wav"
# aptitude install audiofile-tools
$channels = `sfinfo #$wavSrc|grep channel`.split[0].to_i
if $eggSrc
  $eggNames, $eggs = doEggs wav, $wavSrc, $eggSrc
else
  # aptitude install libsox-fmt-mp3
  if $channels == 1
    `sox #$wavSrc -r #{SR} -s -2 -c 1 #{wav}`
  else
    info "Input soundfile #$wavSrc is multichannel (#$channels channels).  Experimental work ahead!"
    `sox  #$wavSrc -r #{SR} -s -2 -c #$channels  #{wav}`
  end
end
GC.start

if $eggSrc
  # marshal dirMarshal, 'eggs', [$eggNames, $eggs]
  info "marshaling eggs, but C-compatibly"
  Dir.chdir(dirMarshal) do
    open('eggs', 'w') {|f|
      f.puts $eggNames.size
      $eggNames.each {|e| f.puts e}
      f.puts $eggs.size
      $eggs.each {|e| f.puts e.join(' ')}
    }
  end
end

info "reading wav"
$wavS16 = `sox #{wav} -r #{SR} -s -2 -c #$channels -t raw -`
($wavS16.size % (2 * $channels)) .should == 0
$wavcsamp = $wavS16.size / (2 * $channels)
quit "no wav file" if $wavcsamp == 0
info "wav has #{$wavcsamp} samples, #{$wavcsamp / SR} seconds, #{($wavcsamp * 4 / 1000000.0).to_i} MB."
info "wav has #$channels channels" if $channels != 1
if $channels == 1
  $rawS16 = [$wavS16]
else
  # De-interleave the String $wavS16 into $channels parts, 2 chars (16 bits) at a time.
  # This is http://en.wikipedia.org/wiki/In-place_matrix_transposition .
  # Expensive and thrash-prone, thus better avoided by instead changing how memory is accessed.
  if nil
    # Too baroque
    shorts = $wavS16 .bytes.to_a .map(&:chr) .each_slice(2).to_a .map(&:join)
    (shorts.size * 2) .should == $wavS16.size
    $rawS16 = shorts.each_slice($channels).to_a.transpose .map(&:join)
  else
    $rawS16 = $wavS16.unpack("s*").each_slice($channels).to_a.transpose .map{|a| a.pack("s*")}
  end
  $rawS16.size .should == $channels
  $rawS16.each {|r| r.size .should == $wavcsamp * 2}
end
$wavS16 = nil

def writeHTK outfile, rgData, sampPeriodHNSU
  rgData.empty? .should == false
  fileFromString outfile, [rgData.size, sampPeriodHNSU, 4 * rgData[0].size, 9].pack('NNnn') + rgData.flatten.pack('g*')
  outfile
end

class Object
  def linearize() self >= 1.0 ? -2e-9 : Math.log((1.0 - self) / (1.0 + self)) end
end

# Last arg 0, 1 means no transform, linear transform
def writeHTKFromQuicknet fileOut, fileIn, sampPeriodHNSU, vectorWidth, kind=0
  # Parse fileIn.  Each line is numbers, whitespace-delimited.
  # Skip first two ints.  Rest of line is usually 2 floats or 14 floats.
  data = File.readlines(fileIn).map {|l| l.split[2..-1].map {|z| z.to_f}}
  data.each {|a| a.size .should == vectorWidth }
  data.map! {|d| d.map! &:linearize } if kind > 0
  writeHTK fileOut, data, sampPeriodHNSU
end

requireGem 'gsl'
def writeHTKWavelet channel, fileOut, sampPeriodHNSU
  cSampWindow = 32
  w = GSL::Wavelet.alloc GSL::Wavelet::DAUBECHIES_CENTERED, 4
  workspace = GSL::Wavelet::Workspace.alloc cSampWindow
  info "wavelet window is #{cSampWindow.to_f / SR * 1000.0} msec."
  sSamp = sampPeriodHNSU / 1e7
  stride = sSamp * SR
  #info "wavelet stride is #{stride} sec, possibly not an exact integer."
  warn "wavelet window #{cSampWindow} is shorter than stride #{stride}, causing some data to be ignored." if cSampWindow < stride

  def hamming(w) Array.new(w) {|n|  0.54 - 0.46 * cos(2.0*PI*n / (w-1.0)) } end
  h = hamming cSampWindow

  r = []
  cSamp = $rawS16[channel].size / 2
  iSamp = 0.0
  while iSamp+cSampWindow < cSamp
    i = iSamp.to_i
    data = $rawS16[channel][i*2 ... (i+cSampWindow)*2].unpack('s*').to_gv
    data.size .should == h.size
    # Convolve data with Hamming window.
    h.each_with_index {|weight,i| data[i] *= weight }
    r << w.transform(data, GSL::Wavelet::FORWARD, workspace).to_a
    iSamp += stride
  end
  writeHTK fileOut, r, sampPeriodHNSU
end

def writeHTKOracle channel, fileOut, sampPeriodHNSU
  cSamp = $rawS16[channel].size / 2
  sSamp = sampPeriodHNSU / 1e7
  cSampWindow = sSamp*SR
  # Each vector in r is 1 element long, the fraction of egged audio samples in that interval.
  r = Array.new(cSamp/cSampWindow) {[0.0]}
  $eggs.each {|sStart,sEnd,*|
    ((sStart*SR).to_i ... (sEnd*SR).to_i) .each {|iSamp|
      r[iSamp/cSampWindow][0] += 1/cSampWindow
    }
  }
  writeHTK fileOut, r, sampPeriodHNSU
end

class Feature
  attr_reader :name, :vectorsize

  def binarydump
    quit "no data for feature '#@name'" if !@data
    info "data is #{@data.size/@vectorsize} slices of #@vectorsize doubles"
    info "rawdata is #{4 * @data.size} chars"
    GC.start
    @name + "\0" +
      [@iColormap.to_f, @period.to_f, @data.size.to_f/@vectorsize, @vectorsize.to_f].packFastFloat +
      @data.packFastFloat
  end

  def initialize channel, icolormap, filename, name, option=nil, weightsFile=nil
    @iColormap = icolormap
    fAlreadyHTK = @iColormap < 0
    filename = htkFromWav channel, @iColormap, filename, weightsFile, option if !fAlreadyHTK
    # icolormaps 0, 1, 2, 3, 4 mean filename is .wav, to convert to respectively FBANK_Z, MFCC_Z, ANN, wavelet, oracle.
    @name, @period, @vectorsize, @data = name, *readHTK(name, filename)
    if !fAlreadyHTK
      @name += [" raw", " loglin"][option] if option
      `rm -f #{filename}` # delete intermediate file, the htk file just computed from filename.wav.
    end
    GC.start
  end

  @@sampPeriod = "100000.0"	# hnsu, hundreds of nanoseconds, i.e. 1e-7 seconds, or decimicroseconds.
  @@ConfigCommon = "\
    USEHAMMING = T
    ZMEANSOURCE = T
    WINDOWSIZE = #{@@sampPeriod.to_f * 8.0}
    NUMCHANS = #$numchans
    HIFREQ = #{SR/2}
    LOFREQ = 0
    SOURCEFORMAT = WAV
    USEPOWER = T
    TARGETRATE = #@@sampPeriod
    PREEMCOEF = 0.97
    "
  @@Config = [
   "TARGETKIND = FBANK_Z
    ",
   "TARGETKIND = MFCC_Z
    NUMCEPS = #$numchans
    ",
   "TARGETKIND = FBANK_D_A_Z
    ",
   "dummy for wavelets
    ",
   "dummy for oracle
    ",
  ]

  @@Hcfg = "/tmp/timeliner_hcopy.cfg"
  @@Outfile = "/tmp/timeliner_htk"
  FeacatWidth = 78 # must be 1/5 of the value "390" which is somehow part of example/trainANN.wts

  def htkFromWav channel, iKind, infile, weightsFile, option=0
    # infile == ".../marshal/mixed.wav"
    return infile if iKind < 0 || iKind >= @@Config.size

    quit "Cannot make oracle feature without easter eggs" if iKind == 4 && !$eggSrc

    # Special cases.  Use GSL instead of HCopy.
    return writeHTKWavelet channel, @@Outfile, @@sampPeriod.to_f if iKind == 3
    return writeHTKOracle  channel, @@Outfile, @@sampPeriod.to_f if iKind == 4

    fileFromString @@Hcfg, @@ConfigCommon + @@Config[iKind]
    `rm -rf #@@Outfile`
    if $channels == 1
      channel .should == 0
      `HCopy -A -T 1 -C #@@Hcfg #{infile} #@@Outfile`
    else
      tmpfile = "/tmp/channeltmp.wav"
      channelfile = "/tmp/channel.wav"
      fileFromString tmpfile, $rawS16[channel]
      `sox -t raw -r #{SR} -s -2 -c 1 #{tmpfile} #{channelfile};
       HCopy -A -T 1 -C #@@Hcfg #{channelfile} #@@Outfile;
       rm -f #{channelfile} #{tmpfile}`
    end
    `rm -f #@@Hcfg`
    if iKind == 2
      quit "config file line lacks .wts weights file" if !weightsFile
      quit "no ANN weights file #{weightsFile}" if !File.exist? weightsFile
      # HCopy just converted .wav to .feaFB.
      # warn `HList -h #@@Outfile | head -7`

      # Convert .feaFB to HTK.
      padfile = "/tmp/timeliner.pad"
      actfile = "/tmp/timeliner.act"
      `rm -f #{padfile} #{actfile};
       feacat -w #{FeacatWidth} -ip htk -op pfile -o #{padfile} -pad 2 #@@Outfile`

      tmp = `grep biasvec #{weightsFile}` .split
      tmp.size .should == 4
      @@Hidden = tmp[1].to_i
      @@QnsWidth = tmp[3].to_i

      # Convert padfile to actfile (aka quicknet), via pretrained ANN weightsFile.
      # `./PadAndFwdAnn.pl TEST feapfile x #{FeacatWidth} 5 300 14 #{weightsFile}`
      tmp = `qnsfwd ftr1_file=#{padfile} ftr1_format=pfile ftr1_ftr_count=#{FeacatWidth} ftr1_window_len=5 window_extent=5 init_weight_file=#{weightsFile} mlp3_input_size=390 mlp3_hidden_size=#@@Hidden mlp3_output_size=#@@QnsWidth activation_file=#{actfile} activation_format=ascii log_file=/tmp/timeliner_qnsfwd.log`
      warn "qnsfwd had output #{tmp}" if tmp.size > 1

      writeHTKFromQuicknet @@Outfile, actfile, @@sampPeriod.to_f, @@QnsWidth, option
      `rm -f #{padfile} #{actfile}`
    end
    @@Outfile
  end

end

cfg .map!(&:split) .map! {|l| [l[0].to_i] + l[1..-1]}			# Parse each line's leading index.
cfg .map! {|l| l[0]<0 ? l : [l[0], wav] + l[1..-1]}			# Insert 'wav' after nonnegative indices.
cfg .map! {|l| l.size <= 3 ? l : l[0..2] + [l[3].to_i] + l[4..-1]}	# Parse optional trailing int and weightsfile.

FeatureFile = "features"
`rm -rf #{dirMarshal}/#{FeatureFile}*`
i = 0
cfg.each {|l|
  GC.start
  $channels.times {|ch|
    marshal dirMarshal, "#{FeatureFile}#{i}", Feature.new(*([ch]+l))
    i += 1
  }
}
info "Input files marshaled."
