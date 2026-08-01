;; Quote / reader forms — R7RS 4.1.2 subset (via eval of datums).

(check-eq "quote symbol" 'a (quote a))
(check-equal "quote list" '(+ 1 2) (quote (+ 1 2)))
(check-equal "quote empty" '() '())
(check-equal "quote nested" '((a) b) (list (list 'a) 'b))
(check-equal "vector literal via vector" (vector 'a 'b) (vector 'a 'b))

(check-equal "string" "abc" "abc")
(check-assert "true" #t)
(check-assert "not false" (not #f))

(check-finish)
