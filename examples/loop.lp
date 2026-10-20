% Positive loop: a and b support each other, only c can start the loop.
{ c }.
a :- b.
b :- a.
a :- c.
#show a/0. #show b/0. #show c/0.
