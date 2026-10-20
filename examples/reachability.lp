% Reachability: choose a subset of the edges of a directed graph and ask
% which nodes are reachable from node 1 through the chosen edges.
% The recursive rule for reach/1 forms positive loops (SCCs) in the ground
% program; only well-founded derivations count (reach(X) must be derived
% from node 1, not from a cycle supporting itself).
node(1..5).
edge(1,2). edge(2,3). edge(3,1). edge(3,4). edge(4,5). edge(5,4).
{ use(X,Y) : edge(X,Y) }.
reach(1).
reach(Y) :- reach(X), use(X,Y).
% Every node must be reachable.
:- node(X), not reach(X).
#show use/2.
