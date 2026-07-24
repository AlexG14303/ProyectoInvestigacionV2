#include "../src/repositories/followup-repository.h"
int main(void) { char q[64]; cf_build_followup_id_filter(1, q, sizeof(q)); return q[0] ? 0 : 1; }
