# GMPIConsumerT example

This example shows how to use the GMPIConsumerT of the geneva library collection to process work items in a cluster
communicating via MPI.  

This example instantiates the GMPIConsumerT directly, which is not very convenient. Therefore,
we advise you to have a look at the GOptimizer class which wraps all consumers including the GMPIConsumerT. Most other examples
use the GOptimizer class, so you might want to look at those.  
This example however shows you how to use GMPIConsumerT directly without GOptimizer and is similar to the example
[06_GDirectEA](../06_GDirectEA).

## Run the example:

The example is best started with a runner program like `mpirun` like so:

```
mpirun -np 4 ./GMPIConsumer
```

The above command will run the example in 4 processes. Each asks for its role first. One of the
processes runs the master node which distributes the work items. The other three processes run as
its clients.

