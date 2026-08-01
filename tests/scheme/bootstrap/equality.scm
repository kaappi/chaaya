;; Equality and types — Kaappi/R7RS 6.1 subset.

(check-assert "eq? symbols" (eq? 'a 'a))
(check-assert "eq? fixnum" (eq? 1 1))
(check-assert "not eq? lists" (not (eq? (list 1) (list 1))))

(check-assert "eqv? fixnum" (eqv? 3 3))
(check-assert "eqv? flonum" (eqv? 1.5 1.5))
(check-assert "eqv? bool" (eqv? #t #t))

(check-assert "equal? string" (equal? "ab" "ab"))
(check-assert "equal? nested" (equal? '(1 (2 3) 4) (list 1 (list 2 3) 4)))
(check-assert "equal? vector" (equal? (vector 1 2) (vector 1 2)))
(check-assert "not equal? vector" (not (equal? (vector 1) (vector 2))))

(check-assert "number? int" (number? 3))
(check-assert "number? float" (number? 3.14))
(check-assert "not number? #t" (not (number? #t)))
(check-assert "symbol?" (symbol? 'foo))
(check-assert "string?" (string? "foo"))
(check-assert "boolean?" (boolean? #f))
(check-assert "vector?" (vector? (vector)))
(check-assert "pair?" (pair? (cons 1 '())))
(check-assert "null?" (null? '()))

(check-eqv "comparisons" #t (< 1 2 3))
(check-eqv "comparisons fail" #f (< 1 3 2))
(check-eqv "=" #t (= 2 2 2))
(check-eqv ">=" #t (>= 3 2 2))

(check-finish)
