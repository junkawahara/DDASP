% Independent sets of a 4x4 grid graph: no two chosen vertices are adjacent.
% There are many solutions; the ZDD counts them without enumerating.
#const n = 4.
vertex((X,Y)) :- X = 1..n, Y = 1..n.
adj((X,Y),(X+1,Y)) :- vertex((X,Y)), X < n.
adj((X,Y),(X,Y+1)) :- vertex((X,Y)), Y < n.
{ in(V) : vertex(V) }.
:- adj(U,V), in(U), in(V).
#show in/1.
