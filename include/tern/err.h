#ifndef TERN_ERR_H
#define TERN_ERR_H

/* Return codes shared by the core and the ports. Zero is success; everything else is negative, so
 * a function that also returns a count can use the same int. */
enum tern_err {
    TERN_OK = 0,
    TERN_EINVAL = -1, /* the arguments are not ones the function accepts */
    TERN_EBUSY = -2,  /* the radio is transmitting, or otherwise cannot do this now */
    TERN_EIO = -3,    /* the hardware did not do what it was told */
    TERN_ESPENT = -4, /* a session has used every counter it has and needs replacing */
};

#endif
