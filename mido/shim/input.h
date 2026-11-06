#pragma once

/* FT5435 touch + PON/GPIO key bring-up test; prints to the console for `seconds`. */
void input_test(unsigned seconds);

/* RPM L10 on, QUP3 clocks, pin mux, touch reset: run before handing off to XNU. */
int input_setup(void);
