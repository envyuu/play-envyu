/* Runs from the firmware's init_array in place of ParamDefs_ctor. RAM is undefined at this point. */
void arp_init(void);
void lr_reset(void);

void mod_init(void) {
    arp_init();      /* calls ParamDefs_ctor, then sets up the arpeggiator */
    lr_reset();
}
