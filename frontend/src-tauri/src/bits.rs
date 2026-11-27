// Bits are packed LSB-first; multi-byte values are little-endian.

#[derive(Default)]
pub struct BitWriter {
    buf: Vec<u8>,
    bit: usize,
}

impl BitWriter {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn write_bits(&mut self, value: u64, count: u32) {
        for i in 0..count {
            if self.bit % 8 == 0 {
                self.buf.push(0);
            }
            if (value >> i) & 1 == 1 {
                *self.buf.last_mut().unwrap() |= 1 << (self.bit % 8);
            }
            self.bit += 1;
        }
    }

    pub fn write_bool(&mut self, v: bool) {
        self.write_bits(v as u64, 1);
    }
    pub fn write_u8(&mut self, v: u8) {
        self.write_bits(v as u64, 8);
    }
    pub fn write_i8(&mut self, v: i8) {
        self.write_u8(v as u8);
    }
    pub fn write_u16(&mut self, v: u16) {
        self.write_bits(v as u64, 16);
    }
    pub fn write_i16(&mut self, v: i16) {
        self.write_u16(v as u16);
    }
    pub fn write_u32(&mut self, v: u32) {
        self.write_bits(v as u64, 32);
    }
    pub fn write_i32(&mut self, v: i32) {
        self.write_u32(v as u32);
    }
    pub fn write_u64(&mut self, v: u64) {
        self.write_bits(v, 64);
    }
    pub fn write_i64(&mut self, v: i64) {
        self.write_u64(v as u64);
    }
    pub fn write_f32(&mut self, v: f32) {
        self.write_u32(v.to_bits());
    }
    pub fn write_f64(&mut self, v: f64) {
        self.write_u64(v.to_bits());
    }
    pub fn write_string(&mut self, s: &str) {
        self.write_u32(s.len() as u32);
        for &b in s.as_bytes() {
            self.write_u8(b);
        }
    }

    pub fn into_bytes(self) -> Vec<u8> {
        self.buf
    }
}

pub struct BitReader<'a> {
    data: &'a [u8],
    bit: usize,
}

impl<'a> BitReader<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Self { data, bit: 0 }
    }

    pub fn read_bits(&mut self, count: u32) -> Option<u64> {
        if self.bit + count as usize > self.data.len() * 8 {
            return None;
        }
        let mut v = 0u64;
        for i in 0..count {
            if (self.data[self.bit / 8] >> (self.bit % 8)) & 1 == 1 {
                v |= 1 << i;
            }
            self.bit += 1;
        }
        Some(v)
    }

    pub fn read_bool(&mut self) -> Option<bool> {
        Some(self.read_bits(1)? == 1)
    }
    pub fn read_u8(&mut self) -> Option<u8> {
        Some(self.read_bits(8)? as u8)
    }
    pub fn read_i8(&mut self) -> Option<i8> {
        Some(self.read_u8()? as i8)
    }
    pub fn read_u16(&mut self) -> Option<u16> {
        Some(self.read_bits(16)? as u16)
    }
    pub fn read_i16(&mut self) -> Option<i16> {
        Some(self.read_u16()? as i16)
    }
    pub fn read_u32(&mut self) -> Option<u32> {
        Some(self.read_bits(32)? as u32)
    }
    pub fn read_i32(&mut self) -> Option<i32> {
        Some(self.read_u32()? as i32)
    }
    pub fn read_u64(&mut self) -> Option<u64> {
        self.read_bits(64)
    }
    pub fn read_i64(&mut self) -> Option<i64> {
        Some(self.read_u64()? as i64)
    }
    pub fn read_f32(&mut self) -> Option<f32> {
        Some(f32::from_bits(self.read_u32()?))
    }
    pub fn read_f64(&mut self) -> Option<f64> {
        Some(f64::from_bits(self.read_u64()?))
    }
    pub fn read_string(&mut self) -> Option<String> {
        let len = self.read_u32()? as usize;
        if self.bit + len * 8 > self.data.len() * 8 {
            return None;
        }
        let bytes = (0..len).map(|_| self.read_u8()).collect::<Option<Vec<u8>>>()?;
        String::from_utf8(bytes).ok()
    }
}
