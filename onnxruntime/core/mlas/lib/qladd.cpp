/*++

Copyright (c) Microsoft Corporation. All rights reserved.

Licensed under the MIT License.

Module Name:

    qladd.cpp

Abstract:

    This module implements routines to quantize linear add.

    For quantization formula as specified in the ONNX operator documentation is:

        Output = Saturate(RoundToEven(Input / Scale) + ZeroPoint)

--*/

#include "qladd.h"

bool
MlasCalcQLinearAddParameters(
    float ScaleRatio_AC, float ScaleRatio_BC,
    int32_t& Shift, int32_t& MultiplierA, int32_t& MultiplierB)
{
    constexpr float MinScaleRatio = 6.103515625e-05f; // std::stof("0x1.0p-14f");
    constexpr float MaxScaleRatio = 256.0f; //std::stof("0x1.0p+8f");
    if (ScaleRatio_AC < MinScaleRatio || ScaleRatio_AC >= MaxScaleRatio ||
            ScaleRatio_BC < MinScaleRatio || ScaleRatio_BC >= MaxScaleRatio) {
        return false;
    }

    const float GreaterScaleRatio = std::max(ScaleRatio_AC, ScaleRatio_BC);
    const int32_t GreaterExponent = (int32_t)(MlasBitsOfFp32(GreaterScaleRatio) >> 23) - 127;
    Shift = 21 - GreaterExponent;
    if (Shift > 31 || Shift < 13) return false;

    const float MultiplierFloatValue = MlasFp32FromBits((uint32_t)(21 - GreaterExponent + 127) << 23);
    MultiplierA = (int32_t) lrintf(ScaleRatio_AC * MultiplierFloatValue);
    MultiplierB = (int32_t) lrintf(ScaleRatio_BC * MultiplierFloatValue);
    return ((MultiplierA < 0x00400000 && MultiplierB < 0x00400000) &&
           (MultiplierA >= 0x00200000 || MultiplierB >= 0x00200000)); // the greater one must fullfil this check
}

// Pure C++ helper, back off here in rare case.
template<typename DataType, bool IsScalarA, bool IsScalarB>
static
MLAS_FORCEINLINE
void
MlasQLinearAddKernelRawHelper(
    const DataType* InputA,
    float ScaleA,
    int32_t ZeroPointA,
    const DataType* InputB,
    float ScaleB,
    int32_t ZeroPointB,
    float ScaleC,
    int32_t ZeroPointC,
    DataType* OutputC,
    size_t N
    )
{

    const float MinimumValue = (float)((int)std::numeric_limits<DataType>::min() - ZeroPointC);
    const float MaximumValue = (float)((int)std::numeric_limits<DataType>::max() - ZeroPointC);

    float ValueA;
    float ValueB;

    if (IsScalarA) {
        ValueA = ScaleA * (int32_t(InputA[0]) - ZeroPointA);
    }
    if (IsScalarB) {
        ValueB = ScaleB * (int32_t(InputB[0]) - ZeroPointB);
    }

    for (size_t n = 0; n < N; n++) {
        if (!IsScalarA) {
            ValueA = ScaleA * (int32_t(InputA[n]) - ZeroPointA);
        }
        if (!IsScalarB) {
            ValueB = ScaleB * (int32_t(InputB[n]) - ZeroPointB);
        }
        float ValueC = (ValueA + ValueB) / ScaleC;
        ValueC = std::min(std::max(ValueC, MinimumValue), MaximumValue);
        OutputC[n] = (DataType)(int32_t)std::nearbyintf(ValueC + ZeroPointC);
    }
}

#if defined(MLAS_NEON_INTRINSICS)

#if ! defined(_MSC_VER)

#define vld1q_s8_ex(pD, align) vld1q_s8((int8_t*)__builtin_assume_aligned(pD, align))
#define vst1_s8_ex(pD, D, align) vst1_s8((int8_t*)__builtin_assume_aligned(pD, align), D)
#define vst1q_s8_ex(pD, D, align) vst1q_s8((int8_t*)__builtin_assume_aligned(pD, align), D)
#define vld1q_u8_ex(pD, align) vld1q_u8((uint8_t*)__builtin_assume_aligned(pD, align))
#define vst1_u8_ex(pD, D, align) vst1_u8((uint8_t*)__builtin_assume_aligned(pD, align), D)
#define vst1q_u8_ex(pD, D, align) vst1q_u8((uint8_t*)__builtin_assume_aligned(pD, align), D)
#define vst1_lane_s8_ex(pD, D, lane, align) vst1_lane_s8((int8_t*)__builtin_assume_aligned(pD, align), D, lane)
#define vst1_lane_u8_ex(pD, D, lane, align) vst1_lane_u8((uint8_t*)__builtin_assume_aligned(pD, align), D, lane)
#define vst1_lane_u32_ex(pD, D, lane, align) vst1_lane_u32((uint32_t*)__builtin_assume_aligned(pD, align), D, lane)
#define vst1_lane_u16_ex(pD, D, lane, align) vst1_lane_u16((uint16_t*)__builtin_assume_aligned(pD, align), D, lane)

#endif

template <typename DataType>
class MLAS_SignedUnsignedIntOps;

template <>
class MLAS_SignedUnsignedIntOps<uint8_t>
{
public:
    typedef uint8_t T;
    typedef uint8x8_t i8x8_t;
    typedef uint8x16_t i8x16_t;
    typedef uint16x8_t i16x8_t;

    static MLAS_FORCEINLINE i8x8_t vmov_n_i8(T value) { return vmov_n_u8(value); }
    static MLAS_FORCEINLINE i8x8_t vget_low_i8(i8x16_t a) { return vget_low_u8(a); }
    static MLAS_FORCEINLINE i8x8_t vget_high_i8(i8x16_t a) { return vget_high_u8(a); }
    static MLAS_FORCEINLINE i16x8_t vsubl_i8(i8x8_t a, i8x8_t b) { return vsubl_u8(a, b); }

    static MLAS_FORCEINLINE int16x8_t vreinterpretq_s16_i16(i16x8_t a) { return vreinterpretq_s16_u16(a); }
    static MLAS_FORCEINLINE uint32x4_t vreinterpretq_u32_i8(i8x16_t a) { return vreinterpretq_u32_u8(a); }
    static MLAS_FORCEINLINE uint16x8_t vreinterpretq_u16_i8(i8x16_t a) { return vreinterpretq_u16_u8(a); }
    static MLAS_FORCEINLINE uint32x2_t vreinterpret_u32_i8(i8x8_t a) { return vreinterpret_u32_u8(a); }
    static MLAS_FORCEINLINE uint16x4_t vreinterpret_u16_i8(i8x8_t a) { return vreinterpret_u16_u8(a); }

    static MLAS_FORCEINLINE i8x16_t vld1q_i8(T const * ptr) { return vld1q_u8_ex(ptr, 1); }
    static MLAS_FORCEINLINE void vst1_i8(T* ptr, i8x8_t a) { vst1_u8_ex(ptr, a, 1); }
    static MLAS_FORCEINLINE void vst1q_i8(T* ptr, i8x16_t a) { vst1q_u8_ex(ptr, a, 1); }

    template <int n>
    static MLAS_FORCEINLINE void vst1_lane_i8(T* ptr, i8x8_t a) { vst1_lane_u8_ex(ptr, a, n, 1); }

    template <int n>
    static MLAS_FORCEINLINE i8x16_t vextq_i8(i8x16_t lo, i8x16_t hi) { return vextq_u8(lo, hi, n); }

    template <int n>
    static MLAS_FORCEINLINE i8x8_t vext_i8(i8x8_t lo, i8x8_t hi) { return vext_u8(lo, hi, n); }

    static MLAS_FORCEINLINE i8x16_t combine_i8_s16(int16x8_t v0, int16x8_t v1)
    {

#if defined(MLAS_NEON64_INTRINSICS)
         return vqmovun_high_s16(vqmovun_s16(v0), v1);
#else
         return vcombine_u8(vqmovun_s16(v0), vqmovun_s16(v1));
#endif

    }
};

template <>
class MLAS_SignedUnsignedIntOps<int8_t>
{
public:
    typedef int8_t T;
    typedef int8x8_t i8x8_t;
    typedef int8x16_t i8x16_t;
    typedef int16x8_t i16x8_t;

    static MLAS_FORCEINLINE i8x8_t vmov_n_i8(T value) { return vmov_n_s8(value); }
    static MLAS_FORCEINLINE i8x8_t vget_low_i8(i8x16_t a) { return vget_low_s8(a); }
    static MLAS_FORCEINLINE i8x8_t vget_high_i8(i8x16_t a) { return vget_high_s8(a); }
    static MLAS_FORCEINLINE i16x8_t vsubl_i8(i8x8_t a, i8x8_t b) { return vsubl_s8(a, b); }

    static MLAS_FORCEINLINE int16x8_t vreinterpretq_s16_i16(i16x8_t a) { return a; }
    static MLAS_FORCEINLINE uint32x4_t vreinterpretq_u32_i8(i8x16_t a) { return vreinterpretq_u32_s8(a); }
    static MLAS_FORCEINLINE uint16x8_t vreinterpretq_u16_i8(i8x16_t a) { return vreinterpretq_u16_s8(a); }
    static MLAS_FORCEINLINE uint32x2_t vreinterpret_u32_i8(i8x8_t a) { return vreinterpret_u32_s8(a); }
    static MLAS_FORCEINLINE uint16x4_t vreinterpret_u16_i8(i8x8_t a) { return vreinterpret_u16_s8(a); }

    static MLAS_FORCEINLINE i8x16_t vld1q_i8(T const * ptr) { return vld1q_s8_ex(ptr, 1); }
    static MLAS_FORCEINLINE void vst1_i8(T* ptr, i8x8_t a) { vst1_s8_ex(ptr, a, 1); }
    static MLAS_FORCEINLINE void vst1q_i8(T* ptr, i8x16_t a) { vst1q_s8_ex(ptr, a, 1); }

    template <int n>
    static MLAS_FORCEINLINE void vst1_lane_i8(T* ptr, i8x8_t a) { vst1_lane_s8_ex(ptr, a, n, 1); }

    template <int n>
    static MLAS_FORCEINLINE i8x16_t vextq_i8(i8x16_t lo, i8x16_t hi) { return vextq_s8(lo, hi, n); }

    template <int n>
    static MLAS_FORCEINLINE i8x8_t vext_i8(i8x8_t lo, i8x8_t hi) { return vext_s8(lo, hi, n); }

    static MLAS_FORCEINLINE i8x16_t combine_i8_s16(int16x8_t v0, int16x8_t v1)
    {

#if defined(MLAS_NEON64_INTRINSICS)
         return vqmovn_high_s16(vqmovn_s16(v0), v1);
#else
         return vcombine_s8(vqmovn_s16(v0), vqmovn_s16(v1));
#endif

    }
};

#if defined(MLAS_NEON64_INTRINSICS)

#define MlasMoveHighS16S32(s16x8) vmovl_high_s16(s16x8)
#define MlasCombineS16S32(lo, hi) vqmovn_high_s32(vqmovn_s32(lo), hi)

#else

#define MlasMoveHighS16S32(s16x8) vmovl_s16(vget_high_s16(s16x8))
#define MlasCombineS16S32(lo, hi) vcombine_s16(vqmovn_s32(lo), vqmovn_s32(hi))

#endif

template<typename DataType, bool IsScalarA, bool IsScalarB>
void
MlasQLinearAddKernelHelper(
    const DataType* InputA,
    float ScaleA,
    int32_t ZeroPointA,
    const DataType* InputB,
    float ScaleB,
    int32_t ZeroPointB,
    float ScaleC,
    int32_t ZeroPointC,
    DataType* OutputC,
    size_t N
    )
{
    typedef MLAS_SignedUnsignedIntOps<DataType> SUI;

    int32_t Shift, MultiplierA, MultiplierB;
    const float ScaleRatio_AC = ScaleA / ScaleC;
    const float ScaleRatio_BC = ScaleB / ScaleC;
    if (!MlasCalcQLinearAddParameters(ScaleRatio_AC, ScaleRatio_BC, Shift, MultiplierA, MultiplierB)) {
        MlasQLinearAddKernelRawHelper<DataType,IsScalarA, IsScalarB>(
            InputA, ScaleA, ZeroPointA, InputB, ScaleB, ZeroPointB, ScaleC, ZeroPointC, OutputC, N);
        return;
    }

    const int32x4_t VectorMultiplierA = vld1q_dup_s32(&MultiplierA);
    const int32x4_t VectorMultiplierB = vld1q_dup_s32(&MultiplierB);
    const typename SUI::i8x8_t VectorZeroPointA = SUI::vmov_n_i8((DataType)ZeroPointA);
    const typename SUI::i8x8_t VectorZeroPointB = SUI::vmov_n_i8((DataType)ZeroPointB);
    const int16x8_t VectorZeroPointC = vmovq_n_s16((int16_t)ZeroPointC);
    const int32x4_t vright_shift = vmovq_n_s32(-Shift); // vld1q_dup_s32(&right_shift);
    const int32x4_t vzero_shift_mask = vreinterpretq_s32_u32(vceqq_s32(vright_shift, vmovq_n_s32(0)));

    int32x4_t vscalar;
    if (IsScalarA) {
        const typename SUI::i8x8_t VectorA0 = SUI::vmov_n_i8(*InputA);
        const int16x8_t va_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(VectorA0, VectorZeroPointA));
        vscalar = vmulq_s32(vmovl_s16(vget_low_s16(va_s16x8)), VectorMultiplierA);
    }
    if (IsScalarB) {
        const typename SUI::i8x8_t VectorB0 = SUI::vmov_n_i8(*InputB);
        const int16x8_t vb_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(VectorB0, VectorZeroPointB));
        vscalar = vmulq_s32(vmovl_s16(vget_low_s16(vb_s16x8)), VectorMultiplierB);
    }
    auto n = static_cast<int64_t>(N);

#if defined(MLAS_NEON64_INTRINSICS)

    while (n >= 32) {
        int32x4_t vacc0_lo, vacc0_hi, vacc1_lo, vacc1_hi, vacc2_lo, vacc2_hi, vacc3_lo, vacc3_hi;
        if (IsScalarA) {
            const typename SUI::i8x16_t VectorB0 = SUI::vld1q_i8(InputB);
            const typename SUI::i8x16_t VectorB1 = SUI::vld1q_i8(InputB + 16);
            InputB += 32;
            const int16x8_t vb0_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_low_i8(VectorB0), VectorZeroPointB));
            const int16x8_t vb1_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_high_i8(VectorB0), VectorZeroPointB));
            const int16x8_t vb2_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_low_i8(VectorB1), VectorZeroPointB));
            const int16x8_t vb3_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_high_i8(VectorB1), VectorZeroPointB));

            vacc0_lo = vmlaq_s32(vscalar, vmovl_s16(vget_low_s16(vb0_s16x8)), VectorMultiplierB);
            vacc1_lo = vmlaq_s32(vscalar, vmovl_s16(vget_low_s16(vb1_s16x8)), VectorMultiplierB);
            vacc2_lo = vmlaq_s32(vscalar, vmovl_s16(vget_low_s16(vb2_s16x8)), VectorMultiplierB);
            vacc3_lo = vmlaq_s32(vscalar, vmovl_s16(vget_low_s16(vb3_s16x8)), VectorMultiplierB);
            vacc0_hi = vmlaq_s32(vscalar, MlasMoveHighS16S32(vb0_s16x8), VectorMultiplierB);
            vacc1_hi = vmlaq_s32(vscalar, MlasMoveHighS16S32(vb1_s16x8), VectorMultiplierB);
            vacc2_hi = vmlaq_s32(vscalar, MlasMoveHighS16S32(vb2_s16x8), VectorMultiplierB);
            vacc3_hi = vmlaq_s32(vscalar, MlasMoveHighS16S32(vb3_s16x8), VectorMultiplierB);
        } else if (IsScalarB) {
            const typename SUI::i8x16_t VectorA0 = SUI::vld1q_i8(InputA);
            const typename SUI::i8x16_t VectorA1 = SUI::vld1q_i8(InputA + 16);
            InputA += 32;
            const int16x8_t va0_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_low_i8(VectorA0), VectorZeroPointA));
            const int16x8_t va1_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_high_i8(VectorA0), VectorZeroPointA));
            const int16x8_t va2_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_low_i8(VectorA1), VectorZeroPointA));
            const int16x8_t va3_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_high_i8(VectorA1), VectorZeroPointA));

            vacc0_lo = vmlaq_s32(vscalar, vmovl_s16(vget_low_s16(va0_s16x8)), VectorMultiplierA);
            vacc1_lo = vmlaq_s32(vscalar, vmovl_s16(vget_low_s16(va1_s16x8)), VectorMultiplierA);
            vacc2_lo = vmlaq_s32(vscalar, vmovl_s16(vget_low_s16(va2_s16x8)), VectorMultiplierA);
            vacc3_lo = vmlaq_s32(vscalar, vmovl_s16(vget_low_s16(va3_s16x8)), VectorMultiplierA);
            vacc0_hi = vmlaq_s32(vscalar, MlasMoveHighS16S32(va0_s16x8), VectorMultiplierA);
            vacc1_hi = vmlaq_s32(vscalar, MlasMoveHighS16S32(va1_s16x8), VectorMultiplierA);
            vacc2_hi = vmlaq_s32(vscalar, MlasMoveHighS16S32(va2_s16x8), VectorMultiplierA);
            vacc3_hi = vmlaq_s32(vscalar, MlasMoveHighS16S32(va3_s16x8), VectorMultiplierA);
        } else  {
            const typename SUI::i8x16_t VectorA0 = SUI::vld1q_i8(InputA);
            const typename SUI::i8x16_t VectorB0 = SUI::vld1q_i8(InputB);
            const typename SUI::i8x16_t VectorA1 = SUI::vld1q_i8(InputA + 16);
            const typename SUI::i8x16_t VectorB1 = SUI::vld1q_i8(InputB + 16);
            InputA += 32;
            InputB += 32;
            const int16x8_t va0_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_low_i8(VectorA0), VectorZeroPointA));
            const int16x8_t vb0_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_low_i8(VectorB0), VectorZeroPointB));
            const int16x8_t va1_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_high_i8(VectorA0), VectorZeroPointA));
            const int16x8_t vb1_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_high_i8(VectorB0), VectorZeroPointB));
            const int16x8_t va2_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_low_i8(VectorA1), VectorZeroPointA));
            const int16x8_t vb2_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_low_i8(VectorB1), VectorZeroPointB));
            const int16x8_t va3_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_high_i8(VectorA1), VectorZeroPointA));
            const int16x8_t vb3_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_high_i8(VectorB1), VectorZeroPointB));

            vacc0_lo = vmulq_s32(vmovl_s16(vget_low_s16(va0_s16x8)), VectorMultiplierA);
            vacc1_lo = vmulq_s32(vmovl_s16(vget_low_s16(va1_s16x8)), VectorMultiplierA);
            vacc2_lo = vmulq_s32(vmovl_s16(vget_low_s16(va2_s16x8)), VectorMultiplierA);
            vacc3_lo = vmulq_s32(vmovl_s16(vget_low_s16(va3_s16x8)), VectorMultiplierA);
            vacc0_hi = vmulq_s32(MlasMoveHighS16S32(va0_s16x8), VectorMultiplierA);
            vacc1_hi = vmulq_s32(MlasMoveHighS16S32(va1_s16x8), VectorMultiplierA);
            vacc2_hi = vmulq_s32(MlasMoveHighS16S32(va2_s16x8), VectorMultiplierA);
            vacc3_hi = vmulq_s32(MlasMoveHighS16S32(va3_s16x8), VectorMultiplierA);

            vacc0_lo = vmlaq_s32(vacc0_lo, vmovl_s16(vget_low_s16(vb0_s16x8)), VectorMultiplierB);
            vacc1_lo = vmlaq_s32(vacc1_lo, vmovl_s16(vget_low_s16(vb1_s16x8)), VectorMultiplierB);
            vacc2_lo = vmlaq_s32(vacc2_lo, vmovl_s16(vget_low_s16(vb2_s16x8)), VectorMultiplierB);
            vacc3_lo = vmlaq_s32(vacc3_lo, vmovl_s16(vget_low_s16(vb3_s16x8)), VectorMultiplierB);
            vacc0_hi = vmlaq_s32(vacc0_hi, MlasMoveHighS16S32(vb0_s16x8), VectorMultiplierB);
            vacc1_hi = vmlaq_s32(vacc1_hi, MlasMoveHighS16S32(vb1_s16x8), VectorMultiplierB);
            vacc2_hi = vmlaq_s32(vacc2_hi, MlasMoveHighS16S32(vb2_s16x8), VectorMultiplierB);
            vacc3_hi = vmlaq_s32(vacc3_hi, MlasMoveHighS16S32(vb3_s16x8), VectorMultiplierB);
        }

        vacc0_lo = vsraq_n_s32(vacc0_lo, vbicq_s32(vacc0_lo, vzero_shift_mask), 31);
        vacc1_lo = vsraq_n_s32(vacc1_lo, vbicq_s32(vacc1_lo, vzero_shift_mask), 31);
        vacc2_lo = vsraq_n_s32(vacc2_lo, vbicq_s32(vacc2_lo, vzero_shift_mask), 31);
        vacc3_lo = vsraq_n_s32(vacc3_lo, vbicq_s32(vacc3_lo, vzero_shift_mask), 31);
        vacc0_hi = vsraq_n_s32(vacc0_hi, vbicq_s32(vacc0_hi, vzero_shift_mask), 31);
        vacc1_hi = vsraq_n_s32(vacc1_hi, vbicq_s32(vacc1_hi, vzero_shift_mask), 31);
        vacc2_hi = vsraq_n_s32(vacc2_hi, vbicq_s32(vacc2_hi, vzero_shift_mask), 31);
        vacc3_hi = vsraq_n_s32(vacc3_hi, vbicq_s32(vacc3_hi, vzero_shift_mask), 31);

        vacc0_lo = vrshlq_s32(vacc0_lo, vright_shift);
        vacc1_lo = vrshlq_s32(vacc1_lo, vright_shift);
        vacc2_lo = vrshlq_s32(vacc2_lo, vright_shift);
        vacc3_lo = vrshlq_s32(vacc3_lo, vright_shift);
        vacc0_hi = vrshlq_s32(vacc0_hi, vright_shift);
        vacc1_hi = vrshlq_s32(vacc1_hi, vright_shift);
        vacc2_hi = vrshlq_s32(vacc2_hi, vright_shift);
        vacc3_hi = vrshlq_s32(vacc3_hi, vright_shift);

        // Pack, saturate, and add output zero point.
        const int16x8_t vacc0 = vqaddq_s16(MlasCombineS16S32(vacc0_lo, vacc0_hi), VectorZeroPointC);
        const int16x8_t vacc1 = vqaddq_s16(MlasCombineS16S32(vacc1_lo, vacc1_hi), VectorZeroPointC);
        const int16x8_t vacc2 = vqaddq_s16(MlasCombineS16S32(vacc2_lo, vacc2_hi), VectorZeroPointC);
        const int16x8_t vacc3 = vqaddq_s16(MlasCombineS16S32(vacc3_lo, vacc3_hi), VectorZeroPointC);

        const typename SUI::i8x16_t vc0 = SUI::combine_i8_s16(vacc0, vacc1);
        const typename SUI::i8x16_t vc1 = SUI::combine_i8_s16(vacc2, vacc3);

        SUI::vst1q_i8(OutputC, vc0);
        SUI::vst1q_i8(OutputC + 16, vc1);
        n -= 32;
        OutputC += 32;
    }

#endif

    typename SUI::i8x16_t vc;
    while (n > 0) {
        int32x4_t vacc0_lo, vacc1_lo, vacc0_hi, vacc1_hi;
        if (IsScalarA) {
            const typename SUI::i8x16_t VectorB0 = SUI::vld1q_i8(InputB);
            InputB += 16;
            const int16x8_t vb0_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_low_i8(VectorB0), VectorZeroPointB));
            const int16x8_t vb1_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_high_i8(VectorB0), VectorZeroPointB));

            vacc0_lo = vmlaq_s32(vscalar, vmovl_s16(vget_low_s16(vb0_s16x8)), VectorMultiplierB);
            vacc1_lo = vmlaq_s32(vscalar, vmovl_s16(vget_low_s16(vb1_s16x8)), VectorMultiplierB);
            vacc0_hi = vmlaq_s32(vscalar, MlasMoveHighS16S32(vb0_s16x8), VectorMultiplierB);
            vacc1_hi = vmlaq_s32(vscalar, MlasMoveHighS16S32(vb1_s16x8), VectorMultiplierB);
        } else if (IsScalarB) {
            const typename SUI::i8x16_t VectorA0 = SUI::vld1q_i8(InputA);
            InputA += 16;
            const int16x8_t va0_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_low_i8(VectorA0), VectorZeroPointA));
            const int16x8_t va1_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_high_i8(VectorA0), VectorZeroPointA));

            vacc0_lo = vmlaq_s32(vscalar, vmovl_s16(vget_low_s16(va0_s16x8)), VectorMultiplierA);
            vacc1_lo = vmlaq_s32(vscalar, vmovl_s16(vget_low_s16(va1_s16x8)), VectorMultiplierA);
            vacc0_hi = vmlaq_s32(vscalar, MlasMoveHighS16S32(va0_s16x8), VectorMultiplierA);
            vacc1_hi = vmlaq_s32(vscalar, MlasMoveHighS16S32(va1_s16x8), VectorMultiplierA);
        } else  {
            const typename SUI::i8x16_t VectorA0 = SUI::vld1q_i8(InputA);
            const typename SUI::i8x16_t VectorB0 = SUI::vld1q_i8(InputB);
            InputA += 16;
            InputB += 16;
            const int16x8_t va0_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_low_i8(VectorA0), VectorZeroPointA));
            const int16x8_t vb0_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_low_i8(VectorB0), VectorZeroPointB));
            const int16x8_t va1_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_high_i8(VectorA0), VectorZeroPointA));
            const int16x8_t vb1_s16x8 = SUI::vreinterpretq_s16_i16(SUI::vsubl_i8(SUI::vget_high_i8(VectorB0), VectorZeroPointB));

            vacc0_lo = vmulq_s32(vmovl_s16(vget_low_s16(va0_s16x8)), VectorMultiplierA);
            vacc1_lo = vmulq_s32(vmovl_s16(vget_low_s16(va1_s16x8)), VectorMultiplierA);
            vacc0_hi = vmulq_s32(MlasMoveHighS16S32(va0_s16x8), VectorMultiplierA);
            vacc1_hi = vmulq_s32(MlasMoveHighS16S32(va1_s16x8), VectorMultiplierA);

            vacc0_lo = vmlaq_s32(vacc0_lo, vmovl_s16(vget_low_s16(vb0_s16x8)), VectorMultiplierB);
            vacc1_lo = vmlaq_s32(vacc1_lo, vmovl_s16(vget_low_s16(vb1_s16x8)), VectorMultiplierB);
            vacc0_hi = vmlaq_s32(vacc0_hi, MlasMoveHighS16S32(vb0_s16x8), VectorMultiplierB);
            vacc1_hi = vmlaq_s32(vacc1_hi, MlasMoveHighS16S32(vb1_s16x8), VectorMultiplierB);
        }

        vacc0_lo = vsraq_n_s32(vacc0_lo, vbicq_s32(vacc0_lo, vzero_shift_mask), 31);
        vacc1_lo = vsraq_n_s32(vacc1_lo, vbicq_s32(vacc1_lo, vzero_shift_mask), 31);
        vacc0_hi = vsraq_n_s32(vacc0_hi, vbicq_s32(vacc0_hi, vzero_shift_mask), 31);
        vacc1_hi = vsraq_n_s32(vacc1_hi, vbicq_s32(vacc1_hi, vzero_shift_mask), 31);

        vacc0_lo = vrshlq_s32(vacc0_lo, vright_shift);
        vacc1_lo = vrshlq_s32(vacc1_lo, vright_shift);
        vacc0_hi = vrshlq_s32(vacc0_hi, vright_shift);
        vacc1_hi = vrshlq_s32(vacc1_hi, vright_shift);

        // Pack, saturate, and add output zero point.
        const int16x8_t vacc0 = vqaddq_s16(vcombine_s16(vqmovn_s32(vacc0_lo), vqmovn_s32(vacc0_hi)), VectorZeroPointC);
        const int16x8_t vacc1 = vqaddq_s16(vcombine_s16(vqmovn_s32(vacc1_lo), vqmovn_s32(vacc1_hi)), VectorZeroPointC);
        vc = SUI::combine_i8_s16(vacc0, vacc1);

        n -= 16;
        if (n < 0) break;

        SUI::vst1q_i8(OutputC, vc);
        OutputC += 16;
    }

    if (n < 0) {
        n += 16;
        typename SUI::i8x8_t i8x8 = SUI::vget_low_i8(vc);
        if (n & 8) {
            SUI::vst1_i8(OutputC, i8x8);
            OutputC += 8;
            i8x8 = SUI::vget_high_i8(vc);
        }
        if (n & 4) {
            vst1_lane_u32_ex((uint32_t*)OutputC, SUI::vreinterpret_u32_i8(i8x8), 0, 1);
            OutputC += 4;
            i8x8 = SUI::template vext_i8<4>(i8x8, i8x8);
        }
        if (n & 2) {
            vst1_lane_u16_ex((uint16_t*)OutputC, SUI::vreinterpret_u16_i8(i8x8), 0, 1);
            OutputC += 2;
            i8x8 = SUI::template vext_i8<2>(i8x8, i8x8);
        }
        if (n & 1) {
            SUI::template vst1_lane_i8<0>(OutputC, i8x8);
        }
    }
}

#elif defined(MLAS_SSE2_INTRINSICS)

template <typename DataType>
MLAS_FORCEINLINE
MLAS_INT32X4
MlasShiftRightInt32(MLAS_INT32X4 v, int imm);

template<> MLAS_INT32X4 MlasShiftRightInt32<int8_t>(MLAS_INT32X4 v, int imm) { return _mm_srai_epi32(v, imm); }
template<> MLAS_INT32X4 MlasShiftRightInt32<uint8_t>(MLAS_INT32X4 v, int imm) { return _mm_srli_epi32(v, imm); }

template <typename DataType>
MLAS_FORCEINLINE
MLAS_INT32X4
MlasUnpackLowI8ToS16(MLAS_INT32X4 v);

template <> MLAS_INT32X4 MlasUnpackLowI8ToS16<int8_t>(MLAS_INT32X4 v) { return _mm_srai_epi16(_mm_unpacklo_epi8(v, v), 8); }
template <> MLAS_INT32X4 MlasUnpackLowI8ToS16<uint8_t>(MLAS_INT32X4 v) { return _mm_srli_epi16(_mm_unpacklo_epi8(v, v), 8); }

template <typename DataType>
MLAS_FORCEINLINE
MLAS_INT32X4
MlasPackS16_128(MLAS_INT32X4 a, MLAS_INT32X4 b);

template <> MLAS_INT32X4 MlasPackS16_128<uint8_t>(MLAS_INT32X4 a, MLAS_INT32X4 b) { return _mm_packus_epi16(a, b); }
template <> MLAS_INT32X4 MlasPackS16_128<int8_t>(MLAS_INT32X4 a, MLAS_INT32X4 b) { return _mm_packs_epi16(a, b); }

template<typename DataType, bool IsScalarA, bool IsScalarB>
void
MlasQLinearAddKernelHelper(
    const DataType* InputA,
    float ScaleA,
    int32_t ZeroPointA,
    const DataType* InputB,
    float ScaleB,
    int32_t ZeroPointB,
    float ScaleC,
    int32_t ZeroPointC,
    DataType* OutputC,
    size_t N
    )
{
    const float ScaleRatio_AC = ScaleA / ScaleC;
    const float ScaleRatio_BC = ScaleB / ScaleC;
    const auto VectorScaleRatio_AC = MlasBroadcastFloat32x4(ScaleRatio_AC);
    const auto VectorScaleRatio_BC = MlasBroadcastFloat32x4(ScaleRatio_BC);
    auto VectorFixedPart = MlasBroadcastFloat32x4((float)ZeroPointC - (ScaleRatio_AC * ZeroPointA + ScaleRatio_BC * ZeroPointB));

    MLAS_FLOAT32X4 va_lo, va_hi, vb_lo, vb_hi;
    if (IsScalarA) {
        va_lo = _mm_set1_ps((float)*InputA);
        VectorFixedPart = _mm_add_ps(VectorFixedPart, _mm_mul_ps(va_lo, VectorScaleRatio_AC));
    }
    if (IsScalarB) {
        vb_lo = _mm_set1_ps((float)*InputB);
        VectorFixedPart = _mm_add_ps(VectorFixedPart, _mm_mul_ps(vb_lo, VectorScaleRatio_BC));
    }

    auto n = static_cast<int64_t>(N);
    MLAS_INT32X4 vc = _mm_setzero_si128();
    while (n > 0) {
        if (!IsScalarA) {
            const auto va_low_half = _mm_loadl_epi64((const MLAS_INT32X4*)InputA);
            const auto va_i16x8 = _mm_unpacklo_epi8(va_low_half, va_low_half);
            InputA += 8;
            va_lo = _mm_cvtepi32_ps(MlasShiftRightInt32<DataType>(_mm_unpacklo_epi16(va_i16x8, va_i16x8), 24));
            va_hi = _mm_cvtepi32_ps(MlasShiftRightInt32<DataType>(_mm_unpackhi_epi16(va_i16x8, va_i16x8), 24));
        }
        if (!IsScalarB) {
            const auto vb_low_half = _mm_loadl_epi64((const MLAS_INT32X4*)InputB);
            const auto vb_i16x8 = _mm_unpacklo_epi8(vb_low_half, vb_low_half);
            InputB += 8;
            vb_lo = _mm_cvtepi32_ps(MlasShiftRightInt32<DataType>(_mm_unpacklo_epi16(vb_i16x8, vb_i16x8), 24));
            vb_hi = _mm_cvtepi32_ps(MlasShiftRightInt32<DataType>(_mm_unpackhi_epi16(vb_i16x8, vb_i16x8), 24));
        }

        MLAS_INT32X4 r_lo, r_hi;
        if (IsScalarA) {
            r_lo = _mm_cvtps_epi32(_mm_add_ps(VectorFixedPart, _mm_mul_ps(vb_lo, VectorScaleRatio_BC)));
            r_hi = _mm_cvtps_epi32(_mm_add_ps(VectorFixedPart, _mm_mul_ps(vb_hi, VectorScaleRatio_BC)));
        } else if (IsScalarB) {
            r_lo = _mm_cvtps_epi32(_mm_add_ps(VectorFixedPart, _mm_mul_ps(va_lo, VectorScaleRatio_AC)));
            r_hi = _mm_cvtps_epi32(_mm_add_ps(VectorFixedPart, _mm_mul_ps(va_hi, VectorScaleRatio_AC)));
        } else {
            r_lo = _mm_cvtps_epi32(_mm_add_ps(_mm_add_ps(VectorFixedPart, _mm_mul_ps(va_lo, VectorScaleRatio_AC)), _mm_mul_ps(vb_lo, VectorScaleRatio_BC)));
            r_hi = _mm_cvtps_epi32(_mm_add_ps(_mm_add_ps(VectorFixedPart, _mm_mul_ps(va_hi, VectorScaleRatio_AC)), _mm_mul_ps(vb_hi, VectorScaleRatio_BC)));
        }
        const auto vc_i16x8 = _mm_packs_epi32(r_lo, r_hi);
        vc = MlasPackS16_128<DataType>(vc_i16x8, vc_i16x8);

        n -= 8;
        if (n < 0) break;

        _mm_storel_epi64((MLAS_INT32X4*)OutputC, vc);
        OutputC += 8;
    }

    if (n < 0) {
        n += 8;
        uint64_t PackedValueC = (uint64_t)_mm_cvtsi128_si64(vc);
        for (int64_t i = 0; i < n; ++i) {
            *((uint8_t*)OutputC + i) = (uint8_t)PackedValueC;
            PackedValueC >>= 8;
        }
    }
}

#else

template<typename DataType, bool IsScalarA, bool IsScalarB>
void
MlasQLinearAddKernelHelper(
    const DataType* InputA,
    float ScaleA,
    int32_t ZeroPointA,
    const DataType* InputB,
    float ScaleB,
    int32_t ZeroPointB,
    float ScaleC,
    int32_t ZeroPointC,
    DataType* OutputC,
    size_t N
    )
{
    // Pure C++ implementation.
    MlasQLinearAddKernelRawHelper<DataType,IsScalarA, IsScalarB>(
        InputA, ScaleA, ZeroPointA, InputB, ScaleB, ZeroPointB, ScaleC, ZeroPointC, OutputC, N);
}

#endif

template<typename DataType>
void
MLASCALL
MlasQLinearAddKernel(
    const DataType* InputA,
    float ScaleA,
    int32_t ZeroPointA,
    const DataType* InputB,
    float ScaleB,
    int32_t ZeroPointB,
    float ScaleC,
    int32_t ZeroPointC,
    DataType* OutputC,
    size_t LengthA,
    size_t LengthB
    )
{
    size_t N = std::max(LengthA, LengthB);
    if (N > 0) {
        if (LengthA == 1) {
            MlasQLinearAddKernelHelper<DataType, true, false>(
                InputA, ScaleA, ZeroPointA, InputB, ScaleB, ZeroPointB, ScaleC, ZeroPointC, OutputC, N);
        } else if (LengthB == 1) {
            MlasQLinearAddKernelHelper<DataType, false, true>(
                InputA, ScaleA, ZeroPointA, InputB, ScaleB, ZeroPointB, ScaleC, ZeroPointC, OutputC, N);
        } else {
            MlasQLinearAddKernelHelper<DataType, false, false>(
                InputA, ScaleA, ZeroPointA, InputB, ScaleB, ZeroPointB, ScaleC, ZeroPointC, OutputC, N);
        }
    }
}

template<>
void
MLASCALL
MlasQLinearAdd<int8_t>(
    const int8_t* InputA,
    float ScaleA,
    int32_t ZeroPointA,
    const int8_t* InputB,
    float ScaleB,
    int32_t ZeroPointB,
    float ScaleC,
    int32_t ZeroPointC,
    int8_t* OutputC,
    size_t LengthA,
    size_t LengthB
    )
{
#if defined(MLAS_TARGET_AMD64)
        MlasPlatform.QLinearAddS8Kernel(
#else
        MlasQLinearAddKernel<int8_t>(
#endif
            InputA, ScaleA, ZeroPointA, InputB, ScaleB, ZeroPointB, ScaleC, ZeroPointC, OutputC, LengthA, LengthB);
}

template<>
void
MLASCALL
MlasQLinearAdd<uint8_t>(
    const uint8_t* InputA,
    float ScaleA,
    int32_t ZeroPointA,
    const uint8_t* InputB,
    float ScaleB,
    int32_t ZeroPointB,
    float ScaleC,
    int32_t ZeroPointC,
    uint8_t* OutputC,
    size_t LengthA,
    size_t LengthB
    )
{
#if defined(MLAS_TARGET_AMD64)
        MlasPlatform.QLinearAddU8Kernel(
#else
        MlasQLinearAddKernel<uint8_t>(
#endif
            InputA, ScaleA, ZeroPointA, InputB, ScaleB, ZeroPointB, ScaleC, ZeroPointC, OutputC, LengthA, LengthB);
}

//
// Function definition for platform usage
//

void
MLASCALL
MlasQLinearAddS8Kernel(
    const int8_t* InputA,
    float ScaleA,
    int32_t ZeroPointA,
    const int8_t* InputB,
    float ScaleB,
    int32_t ZeroPointB,
    float ScaleC,
    int32_t ZeroPointC,
    int8_t* OutputC,
    size_t LengthA,
    size_t LengthB
    )
{
    MlasQLinearAddKernel<int8_t>(
        InputA, ScaleA, ZeroPointA, InputB, ScaleB, ZeroPointB, ScaleC, ZeroPointC, OutputC, LengthA, LengthB);
}

void
MLASCALL
MlasQLinearAddU8Kernel(
    const uint8_t* InputA,
    float ScaleA,
    int32_t ZeroPointA,
    const uint8_t* InputB,
    float ScaleB,
    int32_t ZeroPointB,
    float ScaleC,
    int32_t ZeroPointC,
    uint8_t* OutputC,
    size_t LengthA,
    size_t LengthB
    )
{
    MlasQLinearAddKernel<uint8_t>(
        InputA, ScaleA, ZeroPointA, InputB, ScaleB, ZeroPointB, ScaleC, ZeroPointC, OutputC, LengthA, LengthB);
}

