DirTop = File.dirname "#{Dir.pwd}/#$0"

require 'rubygems'
requireGem 'mmap', 'mmap' # 0.2.6

def fileFromString filename, s
  `rm -rf #{filename}`
  (fd = File.new(filename, "w")).puts s
  fd.close
end

def marshal dir, filename, data=nil
  Dir.chdir(dir) do
    s = "marshal file #{dir}/#{filename}"
    if data
      # Write data to disk.
      if data.respond_to? :binarydump
	fileFromString filename, data.binarydump
      else
	open(filename, "w") {|f| Marshal.dump(data, f)}
      end
      info "wrote #{s}"
    elsif File.exists? filename
      # Read data from disk, and return that.
      _ = nil
      open(filename) {|f| _ = Marshal.load(f)}
      info "read #{s}, of class #{_.class}"
      _
    else
      quit "failed to read #{s}"
    end
  end
end

SR = 16000 # http://corpus.amiproject.org

Sizeof_float = [0.0].pack('g').size

class Array
  def Σ() inject(nil) {|Σ,x| Σ ? x+Σ : x} end
  def mean() Σ.to_f / size end
  def normalize(total) sum = Σ.to_f; map {|x| x/sum*total} end   # Now Σ()==total.
  def minmeanmax() [ self.min, self.mean, self.max ] end
  def dotproduct other
    # size .should == other.size
    dot = 0
    size.times {|i| dot += self[i] * other[i]}
    dot
  end
end

def readHTK name, filename
  info "reading HTK file #{name}."
  begin
    #raw = open(filename, 'rb') {|io| io.read}
    mmap_raw = Mmap::new filename, "r", Mmap::MAP_PRIVATE
    raw = mmap_raw.to_str
  rescue
    warn "no HTK file #{filename}."
    return 1.0, 0, nil
  end

  # 12-byte header, 4-byte float data. http://labrosa.ee.columbia.edu/doc/HTKBook21/node58.html
  nsamps, period_hnsu, bytesPerSamp, parmkind = raw.unpack 'NNnn'
  parmkind_base = parmkind & 077
  parmkind_name = %w(WAVEFORM LPC LPREFC LPCEPSTRA LPDELCEP IREFC MFCC FBANK MELSPEC USER DISCRETE)[parmkind_base]
  parmkind_qualifiers = []
  [ [  0100,'E'],
    [  0200,'N'],  # Requires E D.
    [  0400,'D'],
    [ 01000,'A'], # Requires D.
    [ 02000,'C'], # External file.
    [010000,'K'], # External file.
    [ 04000,'Z'],
    [ 02000,'O']
  ].each {|_| parmkind_qualifiers << "_#{_[1]}" if (parmkind & _[0]) != 0}
  GC.start
  rgz = raw.unpack('g*')[3..-1]
  # Flat array, instead of array of arrays which wastes much memory on temporary objects.
  raw = ""
  mmap_raw.munmap
  GC.start

  quit "header mismatched body" if rgz.size * Sizeof_float != nsamps*bytesPerSamp
  secs = nsamps*period_hnsu / 1e7
  di = bytesPerSamp/4
  if Debug
    info "sample kind #{parmkind_name}#{parmkind_qualifiers}"
    info "#{nsamps} samples == #{secs} sec, #{period_hnsu} hnsu == #{1e7/period_hnsu} Hz, #{di} floats/sample."
  end
  quit "sample length #{bytesPerSamp} not multiple of 4" if bytesPerSamp % 4 != 0
  quit "rgz has #{rgz.size} floats, expected #{bytesPerSamp/4 * nsamps}" if rgz.size != di * nsamps
  info "normalizing #{name}"

  zMin,zMax = rgz.minmax
  dz = zMax-zMin
  rgz.map! {|z| (z-zMin) / dz}

  # Spectrograms (.fb filterbanks) are conventionally black on white, not white on black.
  rgz.map! {|z| 1.0 - z} if filename.downcase =~ /fb/

  info "#{filename} yielded #{rgz.size/di} samples, each with #{di} floats."
  return period_hnsu/1e7, di, rgz
end

# Fast pack().

requireGem 'inline', 'RubyInline'

class Array
  inline do |builder|
    builder.c "
    static void packFastChar() {
      VALUE r = rb_str_buf_new(0);
      const long iLim = RARRAY(self)->len;
      long i = 0;
      for (i=0; i<iLim; ++i) {
        const char c = FIX2LONG(RARRAY(self)->ptr[i]);
	rb_str_buf_cat(r, &c, sizeof(char));
      }
      return r;
    }
    "
  end
rescue
  def packFastChar() pack "C*" end
end

class Array
  inline :C do |builder|
    builder.c "
    static void packFastFloat() {
      const long iLim = RARRAY(self)->len;
      VALUE r = rb_str_buf_new(iLim);
      long i;
      for (i=0; i<iLim; ++i) {
        const float z = NUM2DBL(RARRAY(self)->ptr[i]);
	rb_str_buf_cat(r, (void *)&z, sizeof(float));
      }
      return r;
    }
    "
  end
rescue
  def packFastFloat() pack "f*" end
end
