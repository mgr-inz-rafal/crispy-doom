use std::convert::TryInto;
use std::i32;

type fixed_t = i32;

const FRACBITS: i32 = 16;

#[no_mangle]
pub extern "C" fn FixedMul(a: fixed_t, b: fixed_t) -> fixed_t {
    let value = (a as i64 * b as i64) >> FRACBITS;
    match value.try_into() {
        Ok(value) => value,
        Err(_) => {
            println!("Rust: FixedMul would panic!");
            value as fixed_t
        }
    }
}

#[no_mangle]
pub extern "C" fn FixedDiv(a: fixed_t, b: fixed_t) -> fixed_t {
    if (a.abs() >> 14) >= b.abs() {
        return if a ^ b < 0 {
            fixed_t::MIN
        } else {
            fixed_t::MAX
        };
    }

    let result = ((a as i64) << FRACBITS) / b as i64;
    result.try_into().unwrap()
}
