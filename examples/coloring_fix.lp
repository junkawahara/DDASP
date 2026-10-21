% Second step for coloring.lp, using a stored DD of its colourings:
%   ./build/ddasp --gringo "$G" --save coloring.ddasp examples/coloring.lp
%   ./build/ddasp --gringo "$G" --load coloring.ddasp --enumerate examples/coloring_fix.lp
% The atoms col(N,C) come from the stored DD; no #external declaration is needed.
:- col(1,r).            % node 1 must not be red
red(N) :- col(N,r).     % new atoms derived from the stored ones
#show red/1.            % the stored atoms stay visible anyway
