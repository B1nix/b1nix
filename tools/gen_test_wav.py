import sys
import struct
import math

def generate_wav(filename):
    # Parameters
    sample_rate = 48000
    channels = 2
    bits_per_sample = 16
    duration = 0.05  # 50 ms
    freq = 440
    
    num_samples = int(sample_rate * duration)
    data_size = num_samples * channels * (bits_per_sample // 8)
    file_size = 36 + data_size
    
    # Header
    header = struct.pack(
        '<4sI4s4sIHHIIHH4sI',
        b'RIFF',
        file_size,
        b'WAVE',
        b'fmt ',
        16,  # Subchunk1Size
        1,   # AudioFormat (PCM)
        channels,
        sample_rate,
        int(sample_rate * channels * (bits_per_sample / 8)),  # ByteRate
        int(channels * (bits_per_sample / 8)),  # BlockAlign
        bits_per_sample,
        b'data',
        data_size
    )
    
    # Generate data (sine wave)
    data = bytearray()
    for i in range(num_samples):
        # 440 Hz sine wave
        t = float(i) / sample_rate
        val = int(16000.0 * math.sin(2.0 * math.pi * freq * t))
        # Clamp to 16-bit signed
        val = max(-32768, min(32767, val))
        s_val = struct.pack('<h', val)
        # Stereo: same sample on both channels
        data.extend(s_val)
        data.extend(s_val)
        
    with open(filename, 'wb') as f:
        f.write(header)
        f.write(data)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python gen_test_wav.py <output_file>")
        sys.exit(1)
    generate_wav(sys.argv[1])
