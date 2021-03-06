use ::function_name::named;
use lazy_static::lazy_static;
use std::ops::{Index, IndexMut};
use std::sync::Mutex;

// TODO: The "Random" family of routines is called very frequently
// by the Doom engine, therefore having the mutex overhead
// is not recommended, escpecially since the Doom engine
// is single-threaded. Consider further refactoring.

#[derive(Clone, Copy)]
enum RandomIndices {
    crndindex = 0,
    rndindex = 1,
    prndindex = 2,
}

enum GlobalStateCallResult {
    Int(i32),
    Void,
}

struct RandomIndexArray {
    indices: [usize; 3],
}

impl RandomIndexArray {
    fn new() -> Self {
        Self { indices: [0; 3] }
    }
}

impl Index<RandomIndices> for RandomIndexArray {
    type Output = usize;
    fn index(&self, i: RandomIndices) -> &Self::Output {
        &self.indices[i as usize]
    }
}

impl IndexMut<RandomIndices> for RandomIndexArray {
    fn index_mut(&mut self, i: RandomIndices) -> &mut Self::Output {
        &mut self.indices[i as usize]
    }
}

struct m_random_state {
    indices: RandomIndexArray,
}

impl m_random_state {
    fn get_rndindex() -> i32 {
        7
    }
}

static rndtable: &'static [i32; 256] = &[
    0, 8, 109, 220, 222, 241, 149, 107, 75, 248, 254, 140, 16, 66, 74, 21, 211, 47, 80, 242, 154,
    27, 205, 128, 161, 89, 77, 36, 95, 110, 85, 48, 212, 140, 211, 249, 22, 79, 200, 50, 28, 188,
    52, 140, 202, 120, 68, 145, 62, 70, 184, 190, 91, 197, 152, 224, 149, 104, 25, 178, 252, 182,
    202, 182, 141, 197, 4, 81, 181, 242, 145, 42, 39, 227, 156, 198, 225, 193, 219, 93, 122, 175,
    249, 0, 175, 143, 70, 239, 46, 246, 163, 53, 163, 109, 168, 135, 2, 235, 25, 92, 20, 145, 138,
    77, 69, 166, 78, 176, 173, 212, 166, 113, 94, 161, 41, 50, 239, 49, 111, 164, 70, 60, 2, 37,
    171, 75, 136, 156, 11, 56, 42, 146, 138, 229, 73, 146, 77, 61, 98, 196, 135, 106, 63, 197, 195,
    86, 96, 203, 113, 101, 170, 247, 181, 113, 80, 250, 108, 7, 255, 237, 129, 226, 79, 107, 112,
    166, 103, 241, 24, 223, 239, 120, 198, 58, 60, 82, 128, 3, 184, 66, 143, 224, 145, 224, 81,
    206, 163, 45, 63, 90, 168, 114, 59, 33, 159, 95, 28, 139, 123, 98, 125, 196, 15, 70, 194, 253,
    54, 14, 109, 226, 71, 17, 161, 93, 186, 87, 244, 138, 20, 52, 123, 251, 26, 36, 17, 46, 52,
    231, 232, 76, 31, 221, 84, 37, 216, 165, 212, 106, 197, 242, 98, 43, 39, 175, 254, 145, 190,
    84, 118, 222, 187, 136, 120, 163, 236, 249,
];

#[named]
fn with_global_state<F>(f: F) -> GlobalStateCallResult
where
    F: FnOnce(&mut m_random_state) -> GlobalStateCallResult,
{
    match global_state.lock() {
        Ok(mut guard) => f(&mut guard),
        Err(_) => {
            println!("Rust: lock is poisoned in {}", function_name!());
            return GlobalStateCallResult::Void;
        }
    }
}

lazy_static! {
    static ref global_state: Mutex<m_random_state> = Mutex::new(m_random_state {
        indices: RandomIndexArray::new()
    });
}

fn get_random(state: &mut m_random_state, i: RandomIndices) -> GlobalStateCallResult {
    state.indices[i] = (state.indices[i] + 1) & 0xff;
    GlobalStateCallResult::Int(rndtable[state.indices[i]])
}

fn get_random_by_index(which: RandomIndices) -> i32 {
    if let GlobalStateCallResult::Int(i) = with_global_state(|state| get_random(state, which)) {
        return i;
    }
    println!("Rust: unable to get random number, will return 0");
    0
}

#[no_mangle]
pub extern "C" fn P_Random() -> i32 {
    get_random_by_index(RandomIndices::prndindex)
}

#[no_mangle]
pub extern "C" fn M_Random() -> i32 {
    get_random_by_index(RandomIndices::rndindex)
}

#[no_mangle]
pub extern "C" fn Crispy_Random() -> i32 {
    get_random_by_index(RandomIndices::crndindex)
}

#[no_mangle]
pub extern "C" fn M_ClearRandom() {
    let _ = with_global_state(|state| {
        state.indices = RandomIndexArray::new();
        GlobalStateCallResult::Void
    });
}

#[no_mangle]
pub extern "C" fn P_SubRandom() -> i32 {
    let r = P_Random();
    r - P_Random()
}

#[no_mangle]
pub extern "C" fn Crispy_SubRandom() -> i32 {
    let r = Crispy_Random();
    r - Crispy_Random()
}

#[no_mangle]
pub extern "C" fn GetRndIndex() -> i32 {
    // Hacky, hacky... Doesn't fit into design, but I'll keep it
    // especially since GetRndIndex() is going to be completely
    // refactored out.
    if let GlobalStateCallResult::Int(i) = with_global_state(|state| {
        GlobalStateCallResult::Int(state.indices.indices[RandomIndices::rndindex as usize] as i32)
    }) {
        return i;
    }

    println!("Rust: unable to read 'rndindex', will return 0");
    0
}
