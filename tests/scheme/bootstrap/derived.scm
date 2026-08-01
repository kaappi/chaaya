;; Phase 5: derived forms as C specials (cond, let*, letrec, when, unless, quasiquote)

(check-eqv "cond else" 3
  (cond (#f 1)
        (#f 2)
        (else 3)))

(check-eqv "cond first" 10
  (cond (#t 10)
        (else 20)))

(check-eqv "cond =>" 5
  (cond ((+ 2 3) => (lambda (x) x))
        (else 0)))

(check-eqv "let*" 3
  (let* ((x 1)
         (y (+ x 2)))
    y))

(check-eqv "letrec even?" #t
  (letrec ((even? (lambda (n)
                    (if (= n 0) #t (odd? (- n 1)))))
           (odd? (lambda (n)
                   (if (= n 0) #f (even? (- n 1))))))
    (even? 4)))

(check-eqv "when true" 7
  (let ((x 0))
    (when #t (set! x 7))
    x))

(check-eqv "when false" 0
  (let ((x 0))
    (when #f (set! x 7))
    x))

(check-eqv "unless false" 9
  (let ((x 0))
    (unless #f (set! x 9))
    x))

(check-equal "quasiquote unquote" '(1 2 3)
  `(1 ,(+ 1 1) 3))

(check-equal "quasiquote splice" '(1 2 3 4)
  `(1 ,@(list 2 3) 4))

(check-equal "quasiquote vector" '#(10 5 4 16 9 8)
  (let ((square (lambda (x) (* x x))))
    `#(10 5 ,(square 2) ,@(map square '(4 3)) 8)))

(check-equal "case map and" '((other . z) (semivowel . y) (other . x)
                              (semivowel . w) (vowel . u))
  (map (lambda (x)
         (case x
           ((a e i o u) => (lambda (w) (cons 'vowel w)))
           ((w y) (cons 'semivowel x))
           (else => (lambda (w) (cons 'other w)))))
       '(z y x w u)))

(check-eqv "and short-circuit" #t (and (= 2 2) (> 2 1)))

(check-eqv "named let" 6
  (let loop ((n 3) (acc 0))
    (if (= n 0)
        acc
        (loop (- n 1) (+ acc n)))))

(check-finish)
