void demo_gl(void) {
    uint8_t *fb = (uint8_t*)0xA0000;
    set_mode13h();
    gl_init(320,200,fb);

    vec3 cube[8] = {
        {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
        {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1}
    };
    int faces[12][3] = {
        {0,1,2},{0,2,3},
        {4,5,6},{4,6,7},
        {0,1,5},{0,5,4},
        {2,3,7},{2,7,6},
        {1,2,6},{1,6,5},
        {0,3,7},{0,7,4}
    };

    float ax=0, ay=0, az=0;

    for (;;) {
        gl_clear(0);
        gl_clear_depth();
        gl_matrix_identity();
        gl_matrix_translate(0,0,0);
        gl_matrix_rotate(ax,ay,az);

        for (int i=0;i<12;i++) {
            gl_begin();
            gl_vertex3f(cube[faces[i][0]].x, cube[faces[i][0]].y, cube[faces[i][0]].z);
            gl_vertex3f(cube[faces[i][1]].x, cube[faces[i][1]].y, cube[faces[i][1]].z);
            gl_vertex3f(cube[faces[i][2]].x, cube[faces[i][2]].y, cube[faces[i][2]].z);
            gl_end();
        }

        ax += 0.02f;
        ay += 0.03f;
        az += 0.01f;
    }
}
