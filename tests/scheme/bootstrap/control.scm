;; Control forms — Kaappi/R7RS 4.2 subset available in bootstrap.

(check-eqv "and true" 3 (and 1 2 3))
(check-eqv "and false" #f (and 1 #f 3))
(check-eqv "and empty" #t (and))
(check-eqv "or first" 1 (or 1 2 3))
(check-eqv "or skip false" 9 (or #f #f 9))
(check-eqv "or empty" #f (or))

(check-eqv "if missing alt" #f (if #f 1))
(check-eqv "if only #f false" 1 (if 0 1 2))

(check-eqv "let" 42 (let ((x 40) (y 2)) (+ x y)))
(check-eqv "nested let shadow" 2 (let ((x 1)) (let ((x 2)) x)))
(check-eqv "let outer" 3 (let ((x 1)) (let ((y 2)) (+ x y))))

(check-eqv "begin value" 3 (begin 1 2 3))
(check-eqv "begin with define"
           11
           (begin (define n 10) (+ n 1)))

(check-eqv "not true" #f (not #t))
(check-eqv "not false" #t (not #f))
(check-eqv "not zero" #f (not 0))

(check-finish)
