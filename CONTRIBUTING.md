# Contributing to wayvnc

## Commit Messages

Please, try to write good commit messages. Do your best to follow these 7 rules,
borrowed from [Chris Beams][https://chris.beams.io/posts/git-commit/]:

 1. Separate subject from body with a blank line
 2. Limit the subject line to 50 characters
 3. Capitalize the subject line
 4. Do not end the subject line with a period
 5. Use the imperative mood in the subject line
 6. Wrap the body at 72 characters
 7. Use the body to explain what and why vs. how

If you wish to know why we follow these rules, please read Chris Beams' blog
entry, linked above.

## Style

This project follows the the
[Linux kernel's style guide][https://www.kernel.org/doc/html/latest/process/coding-style.html#codingstyle]
as far as coding style is concererned, with the following exceptions:

 * When declaring pointer variables, the asterisk (`*`) is placed on the left
   with the type rather than the variable name. Declaring multiple variables in
   the same line is not allowed.
 * Wrapped argument lists should not be aligned. Use two tabs instead. There is
   a lot of code that uses aligned argument lists in the project, but I have
   come to the conclusion that these alignments are not very nice to maintain.

## No Brown M&Ms

All pull requests must contain the following sentence in the description:
I have read and understood CONTRIBUTING.md and its associated documents.
