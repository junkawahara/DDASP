% Uses the stored DD written by examples/shift_dd.py (the work patterns built with
% KyotoDD): work(D) is a stored atom, so it may appear in bodies and constraints.
:- not work(sat), not work(sun).            % at least one weekend day
days(N) :- N = #count { D : work(D) }.      % derive the number of working days
#show days/1.
