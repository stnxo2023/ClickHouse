---
description: 'Calculates the approximate number of different values of the argument.'
sidebar_position: 204
slug: /sql-reference/aggregate-functions/reference/uniq
title: 'uniq'
---

# uniq

Calculates the approximate number of different values of the argument.

```sql
uniq(x[, ...])
```

**Arguments**

The function takes a variable number of parameters. Parameters can be `Tuple`, `Array`, `Date`, `DateTime`, `String`, or numeric types.

**Returned value**

- A [UInt64](../../../sql-reference/data-types/int-uint.md)-type number.

**Implementation details**

Function:

- Calculates a hash for all parameters in the aggregate, then uses it in calculations.

- Uses an adaptive sampling algorithm. For the calculation state, the function uses a sample of element hash values up to 65536. This algorithm is very accurate and very efficient on the CPU. When the query contains several of these functions, using `uniq` is almost as fast as using other aggregate functions.

- Provides the result deterministically (it does not depend on the query processing order).

We recommend using this function in almost all scenarios.

**See Also**

- [uniqCombined](/sql-reference/aggregate-functions/reference/uniqcombined)
- [uniqCombined64](/sql-reference/aggregate-functions/reference/uniqcombined64)
- [uniqHLL12](/sql-reference/aggregate-functions/reference/uniqhll12)
- [uniqExact](/sql-reference/aggregate-functions/reference/uniqexact)
- [uniqTheta](/sql-reference/aggregate-functions/reference/uniqthetasketch)
