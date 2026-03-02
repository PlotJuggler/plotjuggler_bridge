# Session Context

## User Prompts

### Prompt 1

Implement the following plan:

# Fix 7 Bugs Found in Code Review

## Context

Code review revealed 7 bugs of varying severity. Two high-severity ref-counting issues (#1, #2) share a root cause: the session's subscription list can be out of sync with the subscription manager's ref counts. The plan fixes all 7, enhances the mock to detect ref-count bugs, and fixes two existing tests that test the wrong thing.

## Step 0: Enhance MockSubscriptionManager with ref counting

The current mock uses `std...

### Prompt 2

double check all your changes with a reviewer agent before the commit

### Prompt 3

commit

### Prompt 4

now, how much coupling do we have between the app and the backends? for instance, if I delete the RTI folder, what will I need to modify in the rest of the repository?

### Prompt 5

if the plans are completed, delete the

