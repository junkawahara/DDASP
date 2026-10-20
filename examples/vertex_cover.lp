% Minimum vertex cover: pick a set of vertices touching every edge, with
% as few vertices as possible. With --opt only the minimum covers remain.
edge(1,2). edge(1,3). edge(2,3). edge(3,4). edge(4,5). edge(4,6). edge(5,6).
vertex(X) :- edge(X,_).
vertex(Y) :- edge(_,Y).
{ in(X) : vertex(X) }.
:- edge(X,Y), not in(X), not in(Y).
#minimize { 1,X : in(X) }.
#show in/1.
