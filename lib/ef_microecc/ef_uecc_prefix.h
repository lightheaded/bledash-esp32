/* Symbol-prefix shim for the vendored micro-ecc.
 *
 * NimBLE-Arduino bundles tinycrypt, which exports the SAME uECC_* linker symbols
 * (an older micro-ecc API, P-256 only). Linking micro-ecc 1.x beside NimBLE then
 * fails with "multiple definition of uECC_make_key/uECC_shared_secret/...".
 *
 * This header renames every micro-ecc FUNCTION symbol to an ef_uECC_* prefix.
 * It is included at the very top of the vendored uECC.h, so the whole micro-ecc
 * translation unit (uECC.c + its .inc files) and every consumer that includes
 * uECC.h see the prefixed names. NimBLE's tinycrypt does not include this header,
 * so it keeps the plain names — no collision.
 *
 * Only function/data symbols are renamed; config macros (uECC_SUPPORTS_*,
 * uECC_WORD_SIZE, ...) and typedefs (uECC_Curve, ...) are intentionally left
 * alone so the -D build flags and the public type names keep working.
 */
#ifndef EF_UECC_PREFIX_H_
#define EF_UECC_PREFIX_H_

/* Public API */
#define uECC_set_rng                 ef_uECC_set_rng
#define uECC_get_rng                 ef_uECC_get_rng
#define uECC_curve_private_key_size  ef_uECC_curve_private_key_size
#define uECC_curve_public_key_size   ef_uECC_curve_public_key_size
#define uECC_make_key                ef_uECC_make_key
#define uECC_shared_secret           ef_uECC_shared_secret
#define uECC_compress                ef_uECC_compress
#define uECC_decompress              ef_uECC_decompress
#define uECC_valid_public_key        ef_uECC_valid_public_key
#define uECC_compute_public_key      ef_uECC_compute_public_key
#define uECC_sign                    ef_uECC_sign
#define uECC_sign_deterministic      ef_uECC_sign_deterministic
#define uECC_verify                  ef_uECC_verify

/* Curve getters */
#define uECC_secp160r1               ef_uECC_secp160r1
#define uECC_secp192r1               ef_uECC_secp192r1
#define uECC_secp224r1               ef_uECC_secp224r1
#define uECC_secp256r1               ef_uECC_secp256r1
#define uECC_secp256k1               ef_uECC_secp256k1

/* Internal externs that also collide / may be exported */
#define uECC_valid_point             ef_uECC_valid_point
#define uECC_point_mult              ef_uECC_point_mult
#define uECC_generate_random_int     ef_uECC_generate_random_int

/* VLI API + curve accessors (external when uECC_ENABLE_VLI_API is set; renaming
 * is harmless otherwise) */
#define uECC_curve_num_words         ef_uECC_curve_num_words
#define uECC_curve_num_bytes         ef_uECC_curve_num_bytes
#define uECC_curve_num_bits          ef_uECC_curve_num_bits
#define uECC_curve_num_n_words       ef_uECC_curve_num_n_words
#define uECC_curve_num_n_bytes       ef_uECC_curve_num_n_bytes
#define uECC_curve_num_n_bits        ef_uECC_curve_num_n_bits
#define uECC_curve_p                 ef_uECC_curve_p
#define uECC_curve_n                 ef_uECC_curve_n
#define uECC_curve_G                 ef_uECC_curve_G
#define uECC_curve_b                 ef_uECC_curve_b
#define uECC_vli_clear               ef_uECC_vli_clear
#define uECC_vli_isZero              ef_uECC_vli_isZero
#define uECC_vli_testBit             ef_uECC_vli_testBit
#define uECC_vli_numBits             ef_uECC_vli_numBits
#define uECC_vli_set                 ef_uECC_vli_set
#define uECC_vli_cmp                 ef_uECC_vli_cmp
#define uECC_vli_rshift1             ef_uECC_vli_rshift1
#define uECC_vli_add                 ef_uECC_vli_add
#define uECC_vli_sub                 ef_uECC_vli_sub
#define uECC_vli_mult                ef_uECC_vli_mult
#define uECC_vli_square              ef_uECC_vli_square
#define uECC_vli_modAdd              ef_uECC_vli_modAdd
#define uECC_vli_modSub              ef_uECC_vli_modSub
#define uECC_vli_mmod                ef_uECC_vli_mmod
#define uECC_vli_mmod_fast           ef_uECC_vli_mmod_fast
#define uECC_vli_modMult             ef_uECC_vli_modMult
#define uECC_vli_modMult_fast        ef_uECC_vli_modMult_fast
#define uECC_vli_modSquare           ef_uECC_vli_modSquare
#define uECC_vli_modSquare_fast      ef_uECC_vli_modSquare_fast
#define uECC_vli_modInv              ef_uECC_vli_modInv
#define uECC_vli_mod_sqrt            ef_uECC_vli_mod_sqrt
#define uECC_vli_equal               ef_uECC_vli_equal
#define uECC_vli_bytesToNative       ef_uECC_vli_bytesToNative
#define uECC_vli_nativeToBytes       ef_uECC_vli_nativeToBytes

#endif  /* EF_UECC_PREFIX_H_ */
