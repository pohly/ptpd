MPICC = mpicc
CFLAGS = -g -O2

timertest: timertest.c
	$(MPICC) $(CFLAGS) -lelf -lm -lrt $< -o $@