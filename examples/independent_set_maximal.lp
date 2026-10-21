% Second step on the stored DD of examples/independent_set.lp: keep only the
% maximal independent sets (every unchosen vertex has a chosen neighbour).
% in/1 is a stored atom: it is used in bodies and constraints, never defined here.
#const grid = 4.
vertex((X,Y)) :- X = 1..grid, Y = 1..grid.
adj((X,Y),(X+1,Y)) :- vertex((X,Y)), X < grid.
adj((X,Y),(X,Y+1)) :- vertex((X,Y)), Y < grid.
blocked(V) :- adj(U,V), in(U).
blocked(U) :- adj(U,V), in(V).
:- vertex(V), not in(V), not blocked(V).
#minimize { 1,V : in(V) }.      % with --opt: the smallest maximal independent sets
#show in/1.
