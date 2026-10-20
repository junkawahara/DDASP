% 0-1 knapsack as a #sum constraint: choose items whose total weight is at
% most the capacity. With --opt the maximum-value selections remain
% (maximising value = minimising its negation).
#const capacity = 10.
item(a, 5, 10).   % item(Name, Weight, Value)
item(b, 4,  7).
item(c, 3,  5).
item(d, 2,  3).
item(e, 1,  1).
{ in(I) : item(I,_,_) }.
:- #sum { W,I : in(I), item(I,W,_) } > capacity.
#minimize { -V,I : in(I), item(I,_,V) }.
#show in/1.
