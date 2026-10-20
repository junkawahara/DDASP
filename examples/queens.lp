% N-queens.
#const n = 6.
{ q(X,Y) : Y = 1..n } = 1 :- X = 1..n.
:- q(X,Y), q(X',Y), X < X'.
:- q(X,Y), q(X',Y'), X < X', |Y - Y'| == X' - X.
#show q/2.
