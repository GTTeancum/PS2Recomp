// Public synthetic block covering loads, delayed stores, integer dependencies,
// and a backward branch with its architectural delay slot.
VU_NATIVE_BLOCK(0x3000u, 8u)
VU_NATIVE_PAIR(0x3000u, 0x01e20800u, 0x000002ffu)
VU_NATIVE_PAIR(0x3008u, 0x00000000u, 0x000002ffu)
VU_NATIVE_PAIR(0x3010u, 0x00000000u, 0x000002ffu)
VU_NATIVE_PAIR(0x3018u, 0x00000000u, 0x000002ffu)
VU_NATIVE_PAIR(0x3020u, 0x03e31000u, 0x000002ffu)
VU_NATIVE_PAIR(0x3028u, 0x100427ffu, 0x000002ffu)
VU_NATIVE_PAIR(0x3030u, 0x5a0027f9u, 0x000002ffu)
VU_NATIVE_PAIR(0x3038u, 0x00000000u, 0x000002ffu)

// Public synthetic pairs used to validate native pair selection and fallback.
VU_NATIVE_PAIR_WORDS(0x00000000u, 0x000002ffu, 3u)
VU_NATIVE_PAIR_WORDS(0x800208f0u, 0x000002ffu, 2u)
VU_NATIVE_PAIR_WORDS(0x00000000u, 0x400002ffu, 1u)
