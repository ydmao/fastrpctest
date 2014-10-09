# fastrpctest #

This project illustrate the usage of fastrpc.

## Build ##
You need to define DEPOBJS, PROTOFILE and include fastrpc/Makefile.include in
you Makefile in order to usage fastrpc. In addition, you need to link $(FASTRPC)
with your binary. See GNUmakefile for an exmaple.

## Performance ##
Test confirms that with old protocol buffer based serialization/deserialization,
the throughput is 5.2 millions echos/second.

This means our blocking based simple parser is ~2X faster (12.7 million echos/second); 
our non-blocking based simple parser is ~5X faster (24.7 million echos/econd).

Usage:
<pre>
  ./bootstrap.sh
  make
  ./obj/server
  ./run.sh
</pre>

See src/app/client.cc for examples about synchronous and asynchronus rpc clients.
