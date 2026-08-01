;; File ports (R7RS 6.13.2 subset)

(define path ".bootstrap_tmp_ports.txt")

(when (file-exists? path)
  (delete-file path))

(check-assert "missing file" (not (file-exists? path)))

(call-with-output-file path
  (lambda (out)
    (display "hello" out)
    (newline out)
    (write 42 out)))

(check-assert "file exists after write" (file-exists? path))

(check-eqv "call-with-input-file read"
           42
           (call-with-input-file path
             (lambda (in)
               (read in)
               (read in))))

(with-output-to-file path
  (lambda ()
    (write 'ok)
    (newline)
    (write 99)))

(check-eqv "with-input-from-file"
           99
           (with-input-from-file path
             (lambda ()
               (read)
               (read))))

(delete-file path)
(check-assert "deleted" (not (file-exists? path)))

(check-finish)
