/* custom error: Cannot execute ScriptUdf over stream/flow inside DQ stage */
USE plato;

$udfScript = @@
import functools
def Len(stream):
    sums = [functools.reduce(lambda x,y: x + y, pair[1], 0) for pair in stream]
    return {"sumByAllVal":functools.reduce(lambda x,y: x + y, sums, 0)}
@@;

$udf = Python3::Len(Callable<(Stream<Tuple<String,Stream<Uint32>>>)->Struct<sumByAllVal:Uint32>>, $udfScript);

--INSERT INTO Output
REDUCE Input1 ON key USING ALL $udf(cast(value as uint32) ?? 0);
