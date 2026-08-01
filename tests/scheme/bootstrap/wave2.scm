;; Wave 2 — apply, lists, strings/vectors, values, ports.

(check-eqv "apply +" 6 (apply + (list 1 2 3)))
(check-eqv "apply rest" 10 (apply + 1 2 (list 3 4)))
(check-eqv "length" 3 (length '(a b c)))
(check-assert "list?" (list? '(1 2)))
(check-assert "not list?" (not (list? '(1 . 2))))
(check-equal "append" '(1 2 3 4) (append '(1 2) '(3 4)))
(check-equal "reverse" '(3 2 1) (reverse '(1 2 3)))
(check-eqv "list-ref" 'c (list-ref '(a b c) 2))
(check-equal "list-tail" '(b c) (list-tail '(a b c) 1))
(check-eqv "cadr" 2 (cadr '(1 2 3)))
(check-equal "map" '(2 3 4) (map (lambda (x) (+ x 1)) '(1 2 3)))

(define sum 0)
(for-each (lambda (x) (set! sum (+ sum x))) '(1 2 3 4))
(check-eqv "for-each" 10 sum)

(check-equal "memq" '(b c) (memq 'b '(a b c)))
(check-equal "assq" '(b 2) (assq 'b '((a 1) (b 2))))

(check-eqv "string-length" 3 (string-length "abc"))
(check-eqv "string-ref" #\b (string-ref "abc" 1))
(check-equal "string-append" "abcdef" (string-append "abc" "def"))
(check-equal "substring" "bc" (substring "abcd" 1 3))
(check-assert "string=?" (string=? "hi" "hi"))
(check-equal "symbol->string" "foo" (symbol->string 'foo))
(check-eq "string->symbol" 'bar (string->symbol "bar"))

(check-eqv "vector-ref" 2 (vector-ref (vector 1 2 3) 1))
(check-equal "vector->list" '(1 2) (vector->list (vector 1 2)))
(check-equal "list->vector" (vector 1 2) (list->vector '(1 2)))

(check-eqv "values single" 3 (values 3))
(check-eqv "call-with-values"
           3
           (call-with-values (lambda () (values 1 2)) (lambda (a b) (+ a b))))

(check-assert "zero?" (zero? 0))
(check-assert "integer?" (integer? 42))
(check-assert "char?" (char? #\a))

(define op (open-output-string))
(display "hi" op)
(newline op)
(write 'x op)
(check-equal "string port" "hi\nx" (get-output-string op))

(define ip (open-input-string "(a b c)"))
(check-equal "read" '(a b c) (read ip))
(check-assert "eof" (eof-object? (read (open-input-string ""))))

(check-assert "port?" (port? (current-output-port)))
(check-assert "input-port?" (input-port? (current-input-port)))

(check-finish)
