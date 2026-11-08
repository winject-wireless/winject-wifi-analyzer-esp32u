Independent DSSS/CCK complex signed-8-bit IQ test vectors from libcrafter,
revision 26347a513bfbf0f5f23a330a83c87ce1ba4667fc,
https://github.com/pellegre/libcrafter/tree/26347a513bfbf0f5f23a330a83c87ce1ba4667fc/crafter/tests/fixtures/iq

Copyright (c) 2012-present Esteban Pellegrino. Distributed under the MIT
license, reproduced in LICENSE.txt. Files suffixed -40msps were resampled
from the upstream 20 MS/s vectors with scipy.signal.resample_poly(x, 2, 1)
and quantized to complex signed 8-bit. No capture gaps are bridged.
Companion .mpdu files are the final PSDU bits from the upstream JSON metadata,
packed least-significant bit first; tests compare the exact decoded bytes.
