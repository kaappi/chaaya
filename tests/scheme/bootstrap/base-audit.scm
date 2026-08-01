;; Phase 7A base audit: high-impact scheme.base procedures and derived forms.

(check-eqv "do sum" 15
  (do ((i 1 (+ i 1))
       (acc 0 (+ acc i)))
      ((> i 5) acc)))

(check-eqv "do commands" 10
  (let ((x 0))
    (do ((i 0 (+ i 1)))
        ((= i 5) x)
      (set! x (+ x 2)))))

(check-eqv "do implicit step" 3
  (do ((x 3))
      ((= x 3) x)))

(define lst (list 1 2 3))
(define lst-copy (list-copy lst))
(check-equal "list-copy value" '(1 2 3) lst-copy)
(check-assert "list-copy fresh pair" (not (eq? lst lst-copy)))
(check-equal "list-copy dotted" '(a b . c) (list-copy '(a b . c)))

(define mlist (list 1 2 3))
(list-set! mlist 1 9)
(check-equal "list-set!" '(1 9 3) mlist)

(check-equal "vector-copy full" (vector 1 2 3) (vector-copy (vector 1 2 3)))
(check-equal "vector-copy range" (vector 2 3) (vector-copy (vector 1 2 3 4) 1 3))
(check-equal "vector-append" (vector 1 2 3 4)
  (vector-append (vector 1 2) (vector 3 4)))

(define vf (vector 1 2 3 4))
(vector-fill! vf 7 1 3)
(check-equal "vector-fill! range" (vector 1 7 7 4) vf)

(check-equal "vector-map one" (vector 2 3 4)
  (vector-map (lambda (x) (+ x 1)) (vector 1 2 3)))
(check-equal "vector-map many" (vector 11 22)
  (vector-map (lambda (a b) (+ a b)) (vector 1 2 3) (vector 10 20)))

(check-equal "substring" "ell" (substring "hello" 1 4))
(check-equal "string-map one" "bcd"
  (string-map (lambda (c) (integer->char (+ 1 (char->integer c)))) "abc"))
(check-equal "string-map many" "XY"
  (string-map (lambda (a b) b) "ab" "XYZ"))

(check-assert "port?" (port? (current-output-port)))
(check-assert "input-port?" (input-port? (current-input-port)))
(check-assert "output-port?" (output-port? (current-output-port)))

(define op (open-output-string))
(check-eqv "call-with-port result" 42
  (call-with-port op (lambda (p) (display "ok" p) 42)))
(check-equal "call-with-port output" "ok" (get-output-string op))

(define fp (open-output-string))
(display "flush" fp)
(flush-output-port fp)
(check-equal "flush-output-port explicit" "flush" (get-output-string fp))
(flush-output-port)

(check-eqv "reader #; datum comment" 2
  (read (open-input-string "#;(+ 1 1) 2")))
(check-equal "reader #; in list" '(a c)
  (read (open-input-string "(a #;b c)")))

(check-eqv "unicode identifier" 7
  (let ((λ 7)) λ))

(check-finish)
