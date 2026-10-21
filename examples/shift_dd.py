"""Build a family of work patterns directly with KyotoDD and store it for ASP.

The family: the sets of weekdays with at most 4 working days and no 3 working
days in a row. Usage:

    .venv/bin/python examples/shift_dd.py shift.ddasp
    ./build/ddasp --gringo "$G" --load shift.ddasp --enumerate examples/shift.lp
"""

import sys

import ddasp
import kyotodd

days = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]

kyotodd.init()
var = dict(zip(days, kyotodd.new_vars(len(days))))  # one ZDD variable per day

family = kyotodd.ZDD.power_set(len(days)).size_le(4)  # at most 4 working days
for i in range(len(days) - 2):  # no 3 working days in a row
    three = kyotodd.ZDD.single_set([var[days[i]], var[days[i + 1]], var[days[i + 2]]])
    family = family - family.restrict(three)  # restrict(g): the sets that contain a set of g

dd = ddasp.StoredDD.from_zdd(family, {var[d]: f"work({d})" for d in days})
print(f"{dd.count} work patterns, {len(dd.names)} atoms: {' '.join(dd.names)}")
dd.save(sys.argv[1] if len(sys.argv) > 1 else "shift.ddasp")
