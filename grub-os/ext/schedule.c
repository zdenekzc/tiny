void schedule() {
    current = current->next;
    switch_to(current);
}
