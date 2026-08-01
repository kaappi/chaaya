;; Numbers — R7RS 6.2 / Kaappi compliance numeric subset for bootstrap.

(check-eqv "+" 6 (+ 1 2 3))
(check-eqv "+ zero args" 0 (+))
(check-eqv "* " 24 (* 2 3 4))
(check-eqv "* zero args" 1 (*))
(check-eqv "unary -" -5 (- 5))
(check-eqv "binary -" 7 (- 10 3))
(check-eqv "variadic -" 5 (- 10 3 2))
(check-assert "/" (= 2 (/ 10 5)))
(check-eqv "unary /" 0.5 (/ 2))

(check-assert "=" (= 1 1 1))
(check-assert "not =" (not (= 1 1 2)))
(check-assert "<" (< 1 2 3))
(check-assert ">" (> 3 2 1))
(check-assert "<=" (<= 1 1 2))
(check-assert ">=" (>= 3 2 2))

(check-assert "number? 0" (number? 0))
(check-assert "number? -1" (number? -1))
(check-assert "number? 3.14" (number? 3.14))
(check-assert "not number? symbol" (not (number? 'n)))

(check-eqv "nested" 14 (+ (* 2 (+ 3 4)) 0))
(check-assert "div mul" (= 5 (* (/ 10 2) 1)))

(check-finish)
