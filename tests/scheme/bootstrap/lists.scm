;; List / pair coverage inspired by Kaappi compliance/lists and R7RS 6.4
;; (bootstrap subset — no apply/map/length builtins).

(check-equal "list" (list 1 2 3) (list 1 2 3))
(check-equal "quote list" '(1 2 3) (list 1 2 3))
(check-assert "null list" (null? (list)))
(check-eqv "car list" 1 (car (list 1 2 3)))
(check-eqv "cadr" 2 (car (cdr (list 1 2 3))))
(check-eqv "caddr" 3 (car (cdr (cdr (list 1 2 3)))))

(define xs (list 10 20 30))
(set-car! xs 11)
(check-eqv "set-car!" 11 (car xs))
(set-cdr! xs (list 21))
(check-eqv "set-cdr! car" 21 (car (cdr xs)))
(check-assert "set-cdr! null end" (null? (cdr (cdr xs))))

(define (list-ref lst n)
  (if (= n 0)
      (car lst)
      (list-ref (cdr lst) (- n 1))))
(check-eqv "list-ref 0" 1 (list-ref '(1 2 3) 0))
(check-eqv "list-ref 2" 3 (list-ref '(1 2 3) 2))

(define (length lst)
  (if (null? lst) 0 (+ 1 (length (cdr lst)))))
(check-eqv "length" 4 (length (list 1 2 3 4)))

(define (append a b)
  (if (null? a) b (cons (car a) (append (cdr a) b))))
(check-equal "append" '(1 2 3 4) (append '(1 2) '(3 4)))

(define (reverse lst)
  (define (loop xs acc)
    (if (null? xs) acc (loop (cdr xs) (cons (car xs) acc))))
  (loop lst '()))
(check-equal "reverse" '(3 2 1) (reverse '(1 2 3)))

(check-assert "equal? lists" (equal? '(a (b) c) (list 'a (list 'b) 'c)))
(check-assert "not equal? lists" (not (equal? '(1 2) '(1 3))))

(check-finish)
