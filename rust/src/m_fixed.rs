use std::convert::TryInto;
use std::i32;

use ::function_name::named;

type fixed_t = i32;

const FRACBITS: fixed_t = 16;

#[named]
#[no_mangle]
pub extern "C" fn FixedMul(a: fixed_t, b: fixed_t) -> fixed_t {
    let value = (a as i64 * b as i64) >> FRACBITS;
    match value.try_into() {
        Ok(value) => value,
        Err(_) => {
            println!("Rust: {} would panic!", function_name!());
            value as fixed_t
        }
    }
}

#[named]
#[no_mangle]
pub extern "C" fn FixedDiv(a: fixed_t, b: fixed_t) -> fixed_t {
    if (a.abs() >> 14) >= b.abs() {
        return if a ^ b < 0 {
            fixed_t::MIN
        } else {
            fixed_t::MAX
        };
    }

    let value = ((a as i64) << FRACBITS) / b as i64;
    match value.try_into() {
        Ok(value) => value,
        Err(_) => {
            println!("Rust: {} would panic!", function_name!());
            value as fixed_t
        }
    }
}
