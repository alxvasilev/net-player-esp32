// Copyright 2018-2019 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License. 

// This is bi quad filter form II for ESP32 processor.
	.text
	.align  4
    .global asmBiquad_f32_df2_mono
    .type   asmBiquad_f32_df2_mono,@function
// The function implements the following C code:
//esp_err_t asmBiquad_f32_df2_mono(const float* input, float* output, int len, float* coef, float* w)
//  {
//    for (int i=0 ; i< len ; i++)
//    {
//        float d0 = input[i] - coef[3]*w[0] - coef[4]*w[1]; (input[i] - a[1]*w[0] - a[2]*w[1];)
//        output[i] = coef[0]*d0 +  coef[1]*w[0] + coef[2]*w[1];
//        w[1] = w[0];
//        w[0] = d0;
//    }
//    return ESP_OK;
//  }

asmBiquad_f32_df2_mono:
// input    - a2
// output   - a3
// len  - a4
// coeffs  - a5
// w- a6

// f0 - b0
// f1 - b1
// f2 - b2
// f3 - a1
// f4 - a2

// f5 - w0
// f6 - w1

	entry	a1, 16
	// Array increment for floating point data should be 4
	lsi   f0, a5, 0
	lsi   f1, a5, 4
	lsi   f2, a5, 8
	lsi   f3, a5, 12
	lsi   f4, a5, 16

	
	neg.s  f5, f3   // -a[1]
	neg.s  f6, f4   // -a[2]

	lsi   f7, a6, 0 // w[0]
	lsi   f8, a6, 4 // w[1]
	
	addi    a3, a3, -4       // i-- // preset a3
	lsi     f9, a2, 0        // f9 = x[i]
	loopnez a4, loop_bq_end_m_ae32
		madd.s  f9, f7, f5   // f9 += -a1*w0
		addi    a3, a3, 4    // out++;
		mul.s   f10, f1, f7  // f10 = b1*w0
		madd.s  f9, f8, f6   // f9 += -a2*w1
		madd.s  f10, f9, f0  // f10 += b0*d0
		addi    a2, a2, 4    // in++;
		madd.s  f10, f2, f8  // f10+= b2*w1, f10 - result
		mov.s   f8, f7       // w1 = w0
		mov.s   f7, f9       // w0 = d0
		lsi     f9, a2,  0   // f9 = x[i]
		ssi	    f10, a3, 0   // y[i] = result
loop_bq_end_m_ae32:
		// Store delay line
		ssi     f7, a6, 0
		ssi     f8, a6, 4

	movi.n	a2, 0 // return status ESP_OK
	retw.n

// =========== Stereo version ===================
.align  4
.global asmBiquad_f32_df2_stereo
.type   asmBiquad_f32_df2_stereo,@function
// void asmBiquad_f32_df2_stereo(const float* input, const float* output, int len, float* coeffs,
//    float* delayL, float* delayR)
// input  : a2
// output : a3
// len    : a4
// coeffs : a5
// delayL : a6
// delayR : a7

asmBiquad_f32_df2_stereo:
    entry	a1, 16
    // Array increment for floating point data should be 4
    lsi   f0, a5, 0  // f0 = b0
    lsi   f1, a5, 4  // f1 = b1
    lsi   f2, a5, 8  // f2 = b2
    lsi   f3, a5, 12 // f3 = a1
    lsi   f4, a5, 16 // f4 = a2

    neg.s  f5, f3   // f5 = -a1
    neg.s  f6, f4   // f6 = -a2

    lsi   f7, a6, 0 // f7 = wL0
    lsi   f8, a6, 4 // f8 = wL1
    lsi   f9, a7, 0 // f9 = wR0
    lsi   f10,a7, 4 // f10 = wR1

    // f11 is d0, f12 is output
    addi  a3, a3, -4        // output--
    lsi   f11, a2, 0        // d0 = *input
    loopnez a4, loop_bq_end_stereo
        madd.s  f11, f5, f7  // d0 += -a1 * wL0
        mul.s   f12, f1, f7  // y = b1 * wL0
        madd.s  f11, f6, f8  // d0 += -a2 * wL1
        addi    a3, a3, 4    // out++
        madd.s  f12, f0, f11 // y += b0 * d0
        addi    a2, a2, 4    // in++
        madd.s  f12, f2, f8  // y += b2 * wL1
        mov.s   f8, f7       // wL1 = wL0
        mov.s   f7, f11      // wL0 = d0
        lsi     f11, a2, 0   // d0 = *input
        ssi	    f12, a3, 0   // *output = result
// right channel
        madd.s  f11, f5, f9  // d0 += -a1 * wR0
        mul.s   f12, f1, f9  // y = b1 * wR0
        madd.s  f11, f6, f10 // d0 += -a2 * wR1
        addi    a3, a3, 4    // out++
        madd.s  f12, f0, f11 // y += b0 * d0
        addi    a2, a2, 4    // in++
        madd.s  f12, f2, f10 // y += b2 * wR1
        mov.s   f10, f9      // wR1 = wR0
        mov.s   f9, f11      // wR0 = d0
        lsi     f11, a2, 0   // d0 = *input
        ssi	    f12, a3, 0   // *output = result
loop_bq_end_stereo:
    // Store delay line
    ssi     f7, a6, 0
    ssi     f8, a6, 4
    ssi     f9, a7, 0
    ssi    f10, a7, 4
    retw.n
