/* C11, OpenGL compatibility profile, FreeGLUT. */
#include <GL/freeglut.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LAYERS=10, PER_LAYER=100, FAN_IN=6,
       NEURONS=LAYERS*PER_LAYER, EDGES=(LAYERS-1)*PER_LAYER*FAN_IN };
typedef struct { float x,y,z,activation,bias; } Neuron;
typedef struct { int from,to; float weight; } Edge;
static Neuron neurons[NEURONS];
static Edge edges[EDGES];
static uint32_t seed=42;
static int width=1280,height=800,paused=0,show_edges=1,drag=0,mx,my;
static int smoke=0,frames=0,failed=0,last_ms;
static float yaw=-24,pitch=17,distance=43;
static double simulation_time;
static int selected=-1,click_x,click_y,moved,scroll_row;
static GLdouble view[16],projection[16];
static GLint viewport[4];
static int camera_ready;

/* Project the ray into object space and intersect the actual neuron spheres.
   Nearest positive intersection wins, including when spheres overlap. */
static int pick(int x,int y) {
    if(!camera_ready) return -1;
    double nx=2.0*x/viewport[2]-1,ny=1-2.0*y/viewport[3];
    double d[3]={nx/projection[0],ny/projection[5],-1};
    double origin[3],direction[3];
    for(int i=0;i<3;i++) {
        origin[i]=-(view[i*4]*view[12]+view[i*4+1]*view[13]+view[i*4+2]*view[14]);
        direction[i]=view[i*4]*d[0]+view[i*4+1]*d[1]+view[i*4+2]*d[2];
    }
    double best=1e30; int hit=-1;
    double a=direction[0]*direction[0]+direction[1]*direction[1]+direction[2]*direction[2];
    for(int n=0;n<NEURONS;n++) {
        Neuron *p=&neurons[n];
        double v[3]={origin[0]-p->x,origin[1]-p->y,origin[2]-p->z};
        double r=0.10+0.055*fabsf(p->activation);
        double b=v[0]*direction[0]+v[1]*direction[1]+v[2]*direction[2];
        double c=v[0]*v[0]+v[1]*v[1]+v[2]*v[2]-r*r,disc=b*b-a*c;
        if(disc<0) continue;
        double t=(-b-sqrt(disc))/a;
        if(t>0 && t<best) { best=t; hit=n; }
    }
    return hit;
}
static void text(int x,int y,const char *s);
static int panel_left(void) { return width>460?width-390:0; }
static void inspector(void) {
    if(selected<0) return;
    int left=panel_left(),x=left+14,y=112;
    glColor3f(0.045f,0.065f,0.105f);
    glBegin(GL_QUADS);
    glVertex2i(left,76); glVertex2i(width,76);
    glVertex2i(width,height-72); glVertex2i(left,height-72); glEnd();
    char line[160]; Neuron *p=&neurons[selected];
    glColor3f(1,0.85f,0.3f);
    snprintf(line,sizeof line,"NEURON %d | L%d / index %d",selected,selected/PER_LAYER,selected%PER_LAYER);
    text(x,y,line); y+=24;
    glColor3f(0.85f,0.9f,1);
    snprintf(line,sizeof line,"Activation: %+.5f",p->activation); text(x,y,line); y+=22;
    snprintf(line,sizeof line,selected<PER_LAYER?"Bias: unused (sine input)":"Bias: %+.5f",p->bias);
    text(x,y,line); y+=22;
    int incoming=0,outgoing=0;
    for(int e=0;e<EDGES;e++) { incoming+=edges[e].to==selected; outgoing+=edges[e].from==selected; }
    snprintf(line,sizeof line,"Incoming: %d   Outgoing: %d",incoming,outgoing); text(x,y,line); y+=26;
    text(x,y,"Dir  Peer neuron  Weight   Contribution"); y+=22;
    int count=incoming+outgoing,rows=(height-110-y)/20;
    if(rows<0) rows=0;
    if(scroll_row>count-rows) scroll_row=count>rows?count-rows:0;
    int row=0;
    for(int e=0;e<EDGES;e++) {
        Edge *c=&edges[e];
        if(c->to!=selected && c->from!=selected) continue;
        int index=row++;
        if(index<scroll_row || index>=scroll_row+rows) continue;
        glColor3f(c->weight>=0?0.3f:1, c->weight>=0?0.85f:0.5f,c->weight>=0?1:0.4f);
        snprintf(line,sizeof line,"%s   %4d     %+.3f    %+.3f",c->to==selected?"IN ":"OUT",
                 c->to==selected?c->from:c->to,c->weight,c->weight*neurons[c->from].activation);
        text(x,y,line); y+=20;
    }
    glColor3f(0.6f,0.7f,0.85f);
    text(x,height-94,"Wheel here: scroll | C: clear");
}

static float random01(void) {
    seed=1664525u*seed+1013904223u;
    return (float)(seed>>8)/16777216.0f;
}
static float clamp(float v,float a,float b) { return fmaxf(a,fminf(b,v)); }

static void build_network(void) {
    seed=42;
    for(int l=0;l<LAYERS;l++) for(int n=0;n<PER_LAYER;n++) {
        Neuron *p=&neurons[l*PER_LAYER+n];
        p->x=(l-(LAYERS-1)*0.5f)*3.0f;
        p->y=((n/10)-4.5f)*0.8f;
        p->z=((n%10)-4.5f)*0.8f;
        p->bias=(random01()-0.5f)*0.4f;
    }
    int e=0;
    for(int l=1;l<LAYERS;l++) for(int n=0;n<PER_LAYER;n++) {
        /* Six distinct inputs per neuron from the preceding layer. */
        int start=(int)(random01()*PER_LAYER);
        for(int k=0;k<FAN_IN;k++) {
            edges[e++]=(Edge){(l-1)*PER_LAYER+(start+k*17)%PER_LAYER,
                              l*PER_LAYER+n,(random01()*2-1)*1.4f};
        }
    }
}

static void forward(double t) {
    for(int n=0;n<PER_LAYER;n++)
        neurons[n].activation=sinf((float)t*1.5f+n*0.17f);
    /* Edges are ordered by destination layer, so each layer reads the
       completed activations of the preceding layer. No training occurs. */
    int e=0;
    for(int l=1;l<LAYERS;l++) for(int n=0;n<PER_LAYER;n++) {
        int dst=l*PER_LAYER+n;
        float sum=neurons[dst].bias;
        for(int k=0;k<FAN_IN;k++,e++)
            sum+=neurons[edges[e].from].activation*edges[e].weight;
        neurons[dst].activation=tanhf(sum);
    }
}

static void text(int x,int y,const char *s) {
    glRasterPos2i(x,y);
    for(;*s;s++) glutBitmapCharacter(GLUT_BITMAP_9_BY_15,*s);
}

static void display(void) {
    glClearColor(0.025f,0.035f,0.065f,1);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    double near=0.1,top=near*tan(45.0*3.141592653589793/360.0);
    double right=top*(double)width/height;
    glFrustum(-right,right,-top,top,near,200);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glTranslatef(0,0,-distance);
    glRotatef(pitch,1,0,0); glRotatef(yaw,0,1,0);
    glGetDoublev(GL_MODELVIEW_MATRIX,view);
    glGetDoublev(GL_PROJECTION_MATRIX,projection);
    glGetIntegerv(GL_VIEWPORT,viewport); camera_ready=1;
    glEnable(GL_DEPTH_TEST);
    if(show_edges) {
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glBegin(GL_LINES);
        for(int e=0;e<EDGES;e++) {
            Edge *c=&edges[e];
            float a=0.025f+0.12f*fabsf(c->weight)/1.4f;
            if(c->weight>=0) glColor4f(0.1f,0.72f,1,a);
            else glColor4f(1,0.3f,0.22f,a);
            Neuron *p=&neurons[c->from],*q=&neurons[c->to];
            glVertex3f(p->x,p->y,p->z); glVertex3f(q->x,q->y,q->z);
        }
        glEnd(); glDepthMask(GL_TRUE); glDisable(GL_BLEND);
    }
    /* Selected edges remain visible when the full connection cloud is hidden. */
    if(selected>=0) {
        glLineWidth(2);
        glBegin(GL_LINES);
        for(int e=0;e<EDGES;e++) {
            Edge *c=&edges[e];
            if(c->from!=selected && c->to!=selected) continue;
            if(c->weight>=0) glColor3f(0.1f,0.85f,1); else glColor3f(1,0.35f,0.2f);
            Neuron *a=&neurons[c->from],*b=&neurons[c->to];
            glVertex3f(a->x,a->y,a->z); glVertex3f(b->x,b->y,b->z);
        }
        glEnd(); glLineWidth(1);
    }
    glEnable(GL_LIGHTING); glEnable(GL_LIGHT0); glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK,GL_AMBIENT_AND_DIFFUSE);
    const GLfloat light[]={-10,15,25,1}; glLightfv(GL_LIGHT0,GL_POSITION,light);
    for(int n=0;n<NEURONS;n++) {
        Neuron *p=&neurons[n];
        float a=fabsf(p->activation);
        if(p->activation>=0) glColor3f(0.18f,0.35f+0.6f*a,0.45f+0.55f*a);
        else glColor3f(0.4f+0.6f*a,0.25f,0.25f+0.2f*a);
        if(n==selected) glColor3f(1,0.85f,0.15f);
        glPushMatrix(); glTranslatef(p->x,p->y,p->z);
        glutSolidSphere(0.10+0.055*a,8,6); glPopMatrix();
    }
    glDisable(GL_LIGHTING); glDisable(GL_COLOR_MATERIAL);
    glColor3f(0.6f,0.7f,0.85f);
    for(int l=0;l<LAYERS;l++) {
        char label[32]; snprintf(label,sizeof label,"L%d / 100",l);
        glRasterPos3f(neurons[l*PER_LAYER].x-0.7f,-4.6f,0);
        for(char *p=label;*p;p++) glutBitmapCharacter(GLUT_BITMAP_HELVETICA_12,*p);
    }
    glDisable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0,width,height,0,-1,1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glColor3f(0.9f,0.94f,1);
    text(24,30,"NEURAL TOPOLOGY | 1,000 neurons | 10 layers | 5,400 connections");
    glColor3f(0.55f,0.68f,0.8f);
    text(24,54,"C / OpenGL | sparse feedforward network | random fixed weights / tanh");
    text(24,height-48,"Click: inspect   Drag: orbit   Wheel: zoom   Space: pause   E: edges   C: clear   Esc: exit");
    inspector();
    text(24,height-24,paused?"PAUSED":"LIVE | cyan: positive / red: negative | neuron size: activation magnitude");
    glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    if(smoke && frames==0) {
        int found=0;
        for(int n=0;n<NEURONS;n++) {
            Neuron *p=&neurons[n]; double eye[4];
            for(int i=0;i<4;i++) eye[i]=view[i]*p->x+view[4+i]*p->y+view[8+i]*p->z+view[12+i];
            int sx=(int)((projection[0]*eye[0]/(-eye[2])+1)*width/2);
            int sy=(int)((1-projection[5]*eye[1]/(-eye[2]))*height/2);
            if(sx<0 || sx>=width || sy<76 || sy>=height-72) continue;
            if(pick(sx,sy)==n) { selected=n; found=1; break; }
        }
        if(!found || pick(-10000,-10000)!=-1) failed=1;
        printf("%s: ray picking and empty-space selection\n",found&&!failed?"PASS":"FAIL");
    }
    if(smoke && frames==2) { selected=550; show_edges=0; }
    if(smoke && glGetError()!=GL_NO_ERROR) failed=1;
    glutSwapBuffers();
    if(smoke && ++frames>=5) glutLeaveMainLoop();
}
static void reshape(int w,int h) {
    width=w>0?w:1; height=h>0?h:1; glViewport(0,0,width,height);
}
static void timer(int unused) {
    (void)unused;
    int now=glutGet(GLUT_ELAPSED_TIME);
    if(!paused) simulation_time+=fmin((now-last_ms)/1000.0,0.1);
    last_ms=now; forward(simulation_time); glutPostRedisplay();
    glutTimerFunc(16,timer,0);
}
static void keyboard(unsigned char key,int x,int y) {
    (void)x; (void)y;
    switch(key) {
    case 27: glutLeaveMainLoop(); break;
    case 'c': case 'C': selected=-1; scroll_row=0; break;
    case ' ': paused=!paused; break;
    case 'e': case 'E': show_edges=!show_edges; break;
    case '+': case '=': distance=clamp(distance-2,12,100); break;
    case '-': distance=clamp(distance+2,12,100); break;
    case 'r': case 'R': yaw=-24; pitch=17; distance=43; simulation_time=0; break;
    }
    glutPostRedisplay();
}
static void mouse(int button,int state,int x,int y) {
    if(button!=GLUT_LEFT_BUTTON) return;
    if(state==GLUT_DOWN) {
        drag=!(selected>=0 && x>=panel_left() && y>=76 && y<height-72);
        mx=click_x=x; my=click_y=y; moved=0;
    } else {
        if(drag && !moved) { selected=pick(x,y); scroll_row=0; }
        drag=0;
    }
    glutPostRedisplay();
}
static void motion(int x,int y) {
    if(abs(x-click_x)>3 || abs(y-click_y)>3) moved=1;
    if(drag && moved) { yaw+=(x-mx)*0.35f; pitch=clamp(pitch+(y-my)*0.35f,-85,85); }
    mx=x; my=y; glutPostRedisplay();
}
static void wheel(int wheel_id,int direction,int x,int y) {
    (void)wheel_id;
    if(selected>=0 && x>=panel_left() && y>=76 && y<height-72) {
        scroll_row-=direction;
        if(scroll_row<0) scroll_row=0;
        glutPostRedisplay(); return;
    }
    distance=clamp(distance-direction*2,12,100); glutPostRedisplay();
}
static int check_network(void) {
    for(int e=0;e<EDGES;e++) {
        Edge *c=&edges[e];
        if(c->from<0 || c->from>=NEURONS || c->to<0 || c->to>=NEURONS ||
           c->to/PER_LAYER!=c->from/PER_LAYER+1) return 1;
        for(int j=e-e%FAN_IN;j<e;j++)
            if(edges[j].from==c->from) return 1;
    }
    for(int step=0;step<100;step++) {
        forward(step*0.1);
        for(int n=0;n<NEURONS;n++)
            if(!isfinite(neurons[n].activation) || fabsf(neurons[n].activation)>1) return 1;
    }
    puts("PASS: 1,000 neurons, 5,400 valid edges, six unique inputs per destination; 100 finite forward passes.");
    return 0;
}
int main(int argc,char **argv) {
    build_network(); forward(0);
    if(argc==2 && !strcmp(argv[1],"--check")) return check_network();
    if(argc==2 && !strcmp(argv[1],"--smoke")) smoke=1;
    else if(argc!=1) { fprintf(stderr,"Usage: %s [--check | --smoke]\n",argv[0]); return 1; }
    /* Keep application-specific switches away from FreeGLUT's parser. */
    int glut_argc=1; glutInit(&glut_argc,argv);
    glutInitContextVersion(2,1);
    glutInitDisplayMode(GLUT_DOUBLE|GLUT_RGBA|GLUT_DEPTH);
    glutInitWindowSize(width,height);
    glutCreateWindow("Neural topology - 1000 neurons");
    glutSetOption(GLUT_ACTION_ON_WINDOW_CLOSE,GLUT_ACTION_GLUTMAINLOOP_RETURNS);
    printf("OpenGL: %s\n",glGetString(GL_VERSION));
    glutDisplayFunc(display); glutReshapeFunc(reshape);
    glutKeyboardFunc(keyboard); glutMouseFunc(mouse);
    glutMotionFunc(motion); glutMouseWheelFunc(wheel);
    last_ms=glutGet(GLUT_ELAPSED_TIME); glutTimerFunc(16,timer,0);
    glutMainLoop();
    if(smoke) printf("%s: rendered %d frames\n",failed?"FAIL":"PASS",frames);
    return failed;
}
