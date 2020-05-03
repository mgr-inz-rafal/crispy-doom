#![allow(
    dead_code,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals
)]
mod m_fixed;
mod m_random;

#[no_mangle]
pub extern "C" fn rs_init() -> i32 {
    println!("Greetings from Rust!");
    0
}
