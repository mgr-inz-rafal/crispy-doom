#![allow(dead_code, non_camel_case_types, non_snake_case)]
mod m_fixed;

#[no_mangle]
pub extern "C" fn rs_init() -> i32 {
    println!("Greetings from Rust!");
    0
}
