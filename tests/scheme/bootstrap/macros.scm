;; Phase 5: hygienic macros (define-syntax / syntax-rules)

(define-syntax my-if
  (syntax-rules ()
    ((my-if test then else)
     (if test then else))))

(check-eqv "my-if true" 1 (my-if #t 1 2))
(check-eqv "my-if false" 2 (my-if #f 1 2))

(define-syntax my-const
  (syntax-rules ()
    ((my-const) 42)))

(check-eqv "my-const" 42 (my-const))

(define-syntax my-begin
  (syntax-rules ()
    ((my-begin e1 e2 ...)
     (begin e1 e2 ...))))

(check-eqv "my-begin ellipsis" 3 (my-begin 1 2 3))
(check-eqv "zero ellipsis" 42 (my-begin 42))

(define-syntax my-list
  (syntax-rules ()
    ((my-list e ...)
     (list e ...))))

(check-equal "my-list ellipsis" '(1 2 3) (my-list 1 2 3))
(check-equal "my-list empty" '() (my-list))

(define-syntax my-and
  (syntax-rules ()
    ((my-and) #t)
    ((my-and x) x)
    ((my-and x y) (if x y #f))))

(check-eqv "my-and nullary" #t (my-and))
(check-eqv "my-and unary" 5 (my-and 5))
(check-eqv "my-and binary true" 3 (my-and 2 3))
(check-eqv "my-and binary false" #f (my-and #f 3))

(define-syntax my-case
  (syntax-rules (is)
    ((my-case x is y)
     (if (= x y) #t #f))))

(check-eqv "my-case match" #t (my-case 3 is 3))
(check-eqv "my-case no match" #f (my-case 3 is 4))

(define-syntax my-swap
  (syntax-rules ()
    ((my-swap a b)
     (let ((tmp a))
       (set! a b)
       (set! b tmp)))))

(check-equal "swap macro" '(20 10)
  (let ((x 10) (y 20))
    (my-swap x y)
    (list x y)))

(define-syntax second
  (syntax-rules ()
    ((second _ x) x)))

(check-eqv "underscore wildcard" 2 (second 1 2))

;; Hygiene: introduced binding should not capture free reference
(define-syntax capture-test
  (syntax-rules ()
    ((capture-test exp)
     (let ((x 100))
       exp))))

(check-eqv "hygiene no capture" 1
  (let ((x 1))
    (capture-test x)))

(check-finish)
