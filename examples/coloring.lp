% 3-coloring of a small graph.
node(1..5).
edge(1,2). edge(2,3). edge(3,4). edge(4,5). edge(5,1). edge(1,3).
color(r). color(g). color(b).
{ col(N,C) : color(C) } = 1 :- node(N).
:- edge(X,Y), col(X,C), col(Y,C).
#show col/2.
