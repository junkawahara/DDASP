% #minimize: at least one of a, b; a costs 1, b costs 2, c costs 1.
{ a ; b ; c }.
:- not a, not b.
#minimize { 1,a : a ; 2,b : b ; 1,c : c }.
#show a/0. #show b/0. #show c/0.
